#include <gp_node.h>

using namespace gp;

/*
 *              PLEASE LOOK at TODOs by searching "TODO" to have an idea of
 *              what is still missing or is improvable!
 */

GaussianProcessNode::GaussianProcessNode (): nh(ros::NodeHandle("gaussian_process")), start(false)
{
    srv_start = nh.advertiseService("start_process", &GaussianProcessNode::cb_start, this);
    //TODO: Added a  publisher to republish point cloud  with new points
    //from  gaussian process,  right now  it's unused  (Fri 06  Nov 2015
    //05:18:41 PM CET -- tabjones)
    pub_model = nh.advertise<pcl::PointCloud<pcl::PointXYZRGB>> ("estimated_model", 1);
    cloud_ptr.reset(new pcl::PointCloud<pcl::PointXYZRGB>);
    hand_ptr.reset(new pcl::PointCloud<pcl::PointXYZRGB>);
    model_ptr.reset(new pcl::PointCloud<pcl::PointXYZRGB>);
}

//Sample some points and query the gp, build a point cloud reconstructed
//model and publish it.
void GaussianProcessNode::sampleAndPublish ()
{
    if (!start){
        ROS_WARN_DELAYED_THROTTLE(80,"[GaussianProcessNode::%s]\tCall start_process service to begin. Not doing anything at the moment...",__func__);
        return;
    }
    Eigen::Vector4f obj_centroid;
    //get centroid of object

    /*****  Query the model with a point  *********************************/
    // Vec3 q(cloud[0]);
    // const double qf = gp->f(q);
    // const double qVar = gp->var(q);
    // std::cout << "y = " << targets[0] << " -> qf = " << qf << " qVar = " << qVar << std::endl << std::endl;
}

//callback to start process service, executes when service is called
bool GaussianProcessNode::cb_start(gp_regression::start_process::Request& req, gp_regression::start_process::Response& res)
{
    if(req.cloud_dir.empty()){
        //Request was empty, means we have to call pacman vision service to
        //get a cloud.
        std::string service_name = nh.resolveName("/pacman_vision/listener/get_cloud_in_hand");
        pacman_vision_comm::get_cloud_in_hand service;
        service.request.save = "false";
        //TODO:  Service requires  to know  which hand  is grasping  the
        //object we  dont have a way to tell inside here.  Assuming it's
        //the right  hand for now.(Fri  06 Nov  2015 05:11:45 PM  CET --
        //tabjones)
        service.request.right = true;
        if (!ros::service::call<pacman_vision_comm::get_cloud_in_hand>(service_name, service))
        {
            ROS_ERROR("[GaussianProcessNode::%s]\tGet cloud in hand service call failed!",__func__);
            return (false);
        }
        //object and hand clouds are saved into class
        pcl::fromROSMsg (service.response.obj, *cloud_ptr);
        pcl::fromROSMsg (service.response.hand, *hand_ptr);
    }
    else{
        //User told us to load a clouds from a dir on disk instead.
        //TODO: Add  some path checks  perhaps? Lets  hope the user  wrote a
        //valid path for now! (Fri 06 Nov 2015 05:03:19 PM CET -- tabjones)
        if (pcl::io::loadPCDFile((req.cloud_dir+"/obj.pcd"), *cloud_ptr) != 0){
            ROS_ERROR("[GaussianProcessNode::%s]\tError loading cloud from %s",__func__,(req.cloud_dir+"obj.pcd").c_str());
            return (false);
        }
        if (pcl::io::loadPCDFile((req.cloud_dir+"/hand.pcd"), *hand_ptr) != 0){
            ROS_ERROR("[GaussianProcessNode::%s]\tError loading cloud from %s",__func__,(req.cloud_dir + "/hand.pcd").c_str());
            return (false);
        }
    }
    return (compute());
}
//gp computation
bool GaussianProcessNode::compute()
{
    if(!cloud_ptr || !hand_ptr){
        //This  should never  happen  if compute  is  called from  start_process
        //service callback, however it does not hurt to add this extra check!
        ROS_ERROR("[GaussianProcessNode::%s]\tObject or Hand cloud pointers are empty. Aborting...",__func__);
        start = false;
        return false;
    }
    Vec3Seq cloud;
    Vec targets;
    const size_t size_cloud = cloud_ptr->size();
    const size_t size_hand = hand_ptr->size();
    targets.resize(size_cloud + size_hand);
    cloud.resize(size_cloud + size_hand);
    if (size_cloud <=0){
        ROS_ERROR("[GaussianProcessNode::%s]\tLoaded object cloud is empty, cannot compute a model. Aborting...",__func__);
        start = false;
        return false;
    }
    for(size_t i=0; i<size_cloud; ++i)
    {
        Vec3 point(cloud_ptr->points[i].x, cloud_ptr->points[i].y, cloud_ptr->points[i].z);
        cloud[i]=point;
        targets[i]=0;
    }
    if (size_hand <=0){
        ROS_WARN("[GaussianProcessNode::%s]\tLoaded hand cloud is empty, using centroid of object as 'external' point for Gaussian model computation...",__func__);
        Eigen::Vector4f centroid;
        if(pcl::compute3DCentroid<pcl::PointXYZRGB>(*cloud_ptr, centroid) == 0){
            ROS_ERROR("[GaussianProcessNode::%s]\tFailed to compute object centroid. Aborting...",__func__);
            start = false;
            return false;
        }
        targets.resize(size_cloud + 1);
        cloud.resize(size_cloud + 1);
        Vec3 ext_point(centroid[0], centroid[1], centroid[2]);
        cloud[size_cloud] = ext_point;
        targets[size_cloud] = 1;
    }
    else{
        for(size_t i=0; i<size_hand; ++i)
        {
            //these points are marked as "external", cause they are from the hand
            Vec3 point(hand_ptr->points[i].x, hand_ptr->points[i].y, hand_ptr->points[i].z);
            cloud[size_cloud +i]=point;
            targets[size_cloud+i]=1;
        }
    }
    /*****  Create the model  *********************************************/
    SampleSet::Ptr trainingData(new SampleSet(cloud, targets));
    LaplaceRegressor::Desc laplaceDesc;
    laplaceDesc.noise = 0.001;
    //create the model to be stored in class
    gp = laplaceDesc.create();
    ROS_INFO("[GaussianProcessNode::%s]\tRegressor created %s",__func__, gp->getName().c_str());
    gp->set(trainingData);
    start = true;
    return true;
}
//update gaussian model with new points from probe
void GaussianProcessNode::update()
{
    // TODO: To  implement entirely. Also  we need a way  to communicate
    // with the probe  package to get the new points.  For instance call
    // this function inside the callback when new points arrive(tabjones
    // on Wednesday 11/11/2015)
}
//Republish cloud method
void GaussianProcessNode::publishCloudModel ()
{
    // TODO:  this  function  is   unused  atm  (tabjones  on  Wednesday
    //11/11/2015).
    //These checks are  to make sure we are not  publishing empty cloud,
    //we have a  gaussian process computed and  there's actually someone
    //who listens to us
    if (start && model_ptr)
        if(!model_ptr->empty() && pub_model.getNumSubscribers()>0)
            pub_model.publish(*model_ptr);
}

