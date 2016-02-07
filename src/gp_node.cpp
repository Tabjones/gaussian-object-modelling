#include <algorithm> //for std::max_element
#include <chrono> //for time measurements

#include <node_utils.hpp>
#include <gp_node.h>

using namespace gp_regression;

/* PLEASE LOOK at  TODOs by searching "TODO" to have an idea  of * what is still
missing or is improvable! */
GaussianProcessNode::GaussianProcessNode (): nh(ros::NodeHandle("gaussian_process")), start(false),
    object_ptr(boost::make_shared<PtC>()), hand_ptr(boost::make_shared<PtC>()),
    model_ptr(boost::make_shared<PtC>()), fake_sampling(false), isAtlas(false), cb_rnd_choose_counter(0)
{
    srv_start = nh.advertiseService("start_process", &GaussianProcessNode::cb_start, this);
    srv_rnd_tests_ = nh.advertiseService("other_rnd_samples", &GaussianProcessNode::cb_rnd_choose, this);
    pub_model = nh.advertise<pcl::PointCloud<pcl::PointXYZRGB>> ("estimated_model", 1);
    pub_markers = nh.advertise<visualization_msgs::MarkerArray> ("atlas", 1);
    // sub_points = nh.subscribe(nh.resolveName("/clicked_point"),1, &GaussianProcessNode::cb_point, this);
    // pub_point = nh.advertise<gp_regression::SampleToExplore> ("sample_to_explore", 0, true);
    // pub_point_marker = nh.advertise<geometry_msgs::PointStamped> ("point_to_explore", 0, true); // TEMP, should be a trajectory, curve, pose
    // pub_direction_marker = nh.advertise<geometry_msgs::WrenchStamped> ("direction_to_explore", 0, true); // TEMP, should be a trajectory, curve, pose
}

void GaussianProcessNode::Publish ()
{
    if (!start){
        ROS_WARN_THROTTLE(60,"[GaussianProcessNode::%s]\tNo object model found! Call start_process service to begin creating a model.",__func__);
        return;
    }
    //publish the model
    publishCloudModel();
    //publish markers
    publishAtlas();
}

// this is a debug callback
bool GaussianProcessNode::cb_rnd_choose(gp_regression::SelectNSamples::Request& req, gp_regression::SelectNSamples::Response& res)
{
        if (!isAtlas){
            ROS_WARN("[GaussianProcessNode::%s]\tNo Atlas created, selecting nothing",__func__);
            return false;
        }
        int N = req.n_selections.data;
        gp_regression::GPProjector<gp_regression::ThinPlate> proj;
        gp_regression::ThinPlate my_kernel(R_);
        reg_.setCovFunction(my_kernel);
        for (int i=0; i < N; ++i)
        {
            int r_id;
            //get a random index
            r_id = getRandIn(0, markers->markers[0].points.size() -1);
            gp_regression::Chart::Ptr gp_chart;
            Eigen::Vector3d c (markers->markers[0].points[r_id].x,
                               markers->markers[0].points[r_id].y,
                               markers->markers[0].points[r_id].z);
            proj.generateChart(reg_, obj_gp, c, gp_chart);
            gp_chart->id = i;
            atlas_.addChart(gp_chart, i);
        }

        // this will create again the same
        createAtlasMarkers();
        // this value is added to the ids of the marker so we won't delete the previous ones
        cb_rnd_choose_counter++;
        return true;
}

//callback to start process service, executes when service is called
bool GaussianProcessNode::cb_start(gp_regression::StartProcess::Request& req, gp_regression::StartProcess::Response& res)
{
    if(req.cloud_dir.empty()){
        //Request was empty, means we have to call pacman vision service to
        //get a cloud.
        std::string service_name = nh.resolveName("/pacman_vision/listener/get_cloud_in_hand");
        pacman_vision_comm::get_cloud_in_hand service;
        // service.request.save = "false";
        ////////////////////////////////////////////////////////////////////////
        //TODO: Service  requires to know which  hand is grasping the  object we
        //dont have a way  to tell inside here. Assuming it's  the left hand for
        //now.(Fri 06 Nov 2015 05:11:45 PM CET -- tabjones)
        ////////////////////////////////////////////////////////////////////////
        service.request.right = false;
        if (!ros::service::call<pacman_vision_comm::get_cloud_in_hand>(service_name, service))
        {
            ROS_ERROR("[GaussianProcessNode::%s]\tGet cloud in hand service call failed!",__func__);
            return (false);
        }
        //object and hand clouds are saved into class
        pcl::fromROSMsg (service.response.obj, *object_ptr);
        pcl::fromROSMsg (service.response.hand, *hand_ptr);
    }
    else{
        if(req.cloud_dir.compare("sphere") == 0 || req.cloud_dir.compare("half_sphere") == 0){
            object_ptr = boost::make_shared<pcl::PointCloud<pcl::PointXYZRGB> >();
            const int ang_div = 24;
            const int lin_div = 20;
            const double radius = 0.06;
            const double ang_step = M_PI * 2 / ang_div;
            const double lin_step = 2 * radius / lin_div;
            double end_lin = radius;
            if (req.cloud_dir.compare("half_sphere")==0)
                end_lin /= 2;
            int j(0);
            for (double lin=-radius+lin_step/2; lin<end_lin; lin+=lin_step)
                for (double ang=0; ang < 2*M_PI; ang+=ang_step, ++j)
                {
                    double x = sqrt(radius*radius - lin*lin) * cos(ang);
                    double y = sqrt(radius*radius - lin*lin) * sin(ang);
                    double z = lin + 1.0; //add translation along z
                    //add sphere points as blue model
                    pcl::PointXYZRGB sp;
                    sp.x = x;
                    sp.y = y;
                    sp.z = z;
                    colorIt(0,0,255, sp);
                    object_ptr->push_back(sp);
                }
        }
        else{
            // User told us to load a clouds from a dir on disk instead.
            if (pcl::io::loadPCDFile((req.cloud_dir+"/obj.pcd"), *object_ptr) != 0){
                ROS_ERROR("[GaussianProcessNode::%s]\tError loading cloud from %s",__func__,(req.cloud_dir+"obj.pcd").c_str());
                return (false);
            }
            if (pcl::io::loadPCDFile((req.cloud_dir+"/hand.pcd"), *hand_ptr) != 0)
                ROS_WARN("[GaussianProcessNode::%s]\tError loading cloud from %s, ignoring hand",__func__,(req.cloud_dir + "/hand.pcd").c_str());
            // We  need  to  fill  point  cloud header  or  ROS  will  complain  when
            // republishing this cloud. Let's assume it was published by asus kinect.
            // I think it's just need the frame id
        }
        object_ptr->header.frame_id="/camera_rgb_optical_frame";
        hand_ptr->header.frame_id="/camera_rgb_optical_frame";
        model_ptr->header.frame_id="/camera_rgb_optical_frame";
    }
    if (computeGP())
        if (computeAtlas())
            return true;
    return false;
}

// TODO: Convert this callback, if  needed, to accept probe points and not
// rviz clicked points, as it is now. (tabjones on Wednesday 18/11/2015)
// Callback for rviz clicked point to simulate probe
/* void GaussianProcessNode::cb_point(const geometry_msgs::PointStamped::ConstPtr &msg)
{
    pcl::PointXYZRGB pt;
    //get the clicked point
    pt.x = msg->point.x;
    pt.y = msg->point.y;
    pt.z = msg->point.z;
    //color it blue
    colorIt(0,0,255, pt);
    model_ptr->push_back(pt);
    object_ptr->push_back(pt);
    Vec3 p(pt.x, pt.y, pt.z);
    //Predispone the sequence to host multiple points, not just one, for the future.
    Vec3Seq points;
    points.push_back(p);
    //update the model
    this->update(points);
}
*/

bool GaussianProcessNode::computeGP()
{
    auto begin_time = std::chrono::high_resolution_clock::now();
    if(!object_ptr){
        //This  should never  happen  if compute  is  called from  start_process
        //service callback, however it does not hurt to add this extra check!
        ROS_ERROR("[GaussianProcessNode::%s]\tObject cloud pointer is empty. Aborting...",__func__);
        start = false;
        return false;
    }
    if (object_ptr->empty()){
        ROS_ERROR("[GaussianProcessNode::%s]\tObject point cloud is empty. Aborting...",__func__);
        start = false;
        return false;
    }
    if (!model_ptr->empty())
        model_ptr->clear();

    /*****  Prepare the training data  *********************************************/
    gp_regression::Data::Ptr cloud_gp = std::make_shared<gp_regression::Data>();

    // Now add centroid as label -1
    Eigen::Vector4f centroid;
    if(pcl::compute3DCentroid<pcl::PointXYZRGB>(*object_ptr, centroid) == 0){
        ROS_ERROR("[GaussianProcessNode::%s]\tFailed to compute object centroid. Aborting...",__func__);
        start = false;
        return false;
    }
    cloud_gp->coord_x.push_back(centroid[0]);
    cloud_gp->coord_y.push_back(centroid[1]);
    cloud_gp->coord_z.push_back(centroid[2]);
    cloud_gp->label.push_back(-1.0);

    // add internal point to rviz in yellow
    pcl::PointXYZRGB cen;
    cen.x = centroid[0];
    cen.y = centroid[1];
    cen.z = centroid[2];
    colorIt(255,0,255, cen);
    model_ptr->push_back(cen);

    // add object points with label 0
    pcl::PointXYZRGB min, max;
    pcl::getMinMax3D(*object_ptr, min, max);
    R_ = (max.x - min.x)*(max.x - min.x) + (max.y - min.y)*(max.y - min.y) + (max.z - min.z)*(max.z - min.z);
    R_ = sqrt(R_);
    std::cout << "R_ = " << R_ << std::endl;

    for(const auto pt : object_ptr->points) {
        cloud_gp->coord_x.push_back(pt.x);
        cloud_gp->coord_y.push_back(pt.y);
        cloud_gp->coord_z.push_back(pt.z);
        cloud_gp->label.push_back(0);
    }
    // add object points to rviz in blue
    *model_ptr += *object_ptr;
    colorThem(0,0,255, model_ptr);

    // TODO: We would probablyy need to add  hand points to the GP in the future.
    // Set them perhaps with target 1 or 0.5 (tabjones on Wednesday 16/12/2015)
    // add hand points to model as slightly different red, if available
    /*if(hand_ptr){
        colorThem(255,125,0, hand_ptr);
        *model_ptr += *hand_ptr;
        for(const auto pt : hand_ptr->points) {
             cloud_gp->coord_x.push_back(pt.x);
             cloud_gp->coord_y.push_back(pt.y);
             cloud_gp->coord_z.push_back(pt.z);
             cloud_gp->label.push_back(0.1);
         }
    }*/
    // I tried, but at least with the offline example it was
    // giving an error. I don't have the force now with me, Luke,
    // maybe another day, but very likely that they are far too many.
    // Thus downsampling the hand cloud could be the answer.
    // (carlosjoserg 7/02/2016)

    // add points in a sphere around centroid with label 1
    // sphere bounds computation
    const int ang_div = 8; //divide 360° in 8 pieces, i.e. steps of 45°
    const int lin_div = 6; //divide diameter into 6 pieces
    // This makes 8*6 = 48 points.
    const double ang_step = M_PI * 2 / ang_div; //steps of 45°
    double radius = 0.15; //0.5 * 2.0;
    const double lin_step = 2 * radius / lin_div;
    // 8 steps for diameter times 6 for angle, make  points on the sphere surface
    int j(0);
    for (double lin=-radius+lin_step/2; lin< radius; lin+=lin_step)
    {
        for (double ang=0; ang < 2*M_PI; ang+=ang_step, ++j)
        {
            // add the centroid everytime as an offset
            double x = sqrt(radius*radius - lin*lin) * cos(ang) + centroid[0];
            double y = sqrt(radius*radius - lin*lin) * sin(ang) + centroid[1];
            double z = lin + centroid[2];

            cloud_gp->coord_x.push_back(x);
            cloud_gp->coord_y.push_back(y);
            cloud_gp->coord_z.push_back(z);
            cloud_gp->label.push_back(1.0);

            // add sphere points to rviz in red
            pcl::PointXYZRGB sp;
            sp.x = x;
            sp.y = y;
            sp.z = z;
            colorIt(255,0,0, sp);
            model_ptr->push_back(sp);
        }
    }
    /*****  Create the gp model  *********************************************/
    //create the model to be stored in class
    if (cloud_gp->coord_x.size() != cloud_gp->label.size()){
        ROS_ERROR("[GaussianProcessNode::%s]\tTargets Points size mismatch, something went wrong. Aborting...",__func__);
        start = false;
        return false;
    }

    obj_gp = std::make_shared<gp_regression::Model>();
    gp_regression::ThinPlate my_kernel(R_);
    reg_.setCovFunction(my_kernel);
    const bool withNormals = false;
    reg_.create<withNormals>(cloud_gp, obj_gp);
    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - begin_time).count();
    ROS_INFO("[GaussianProcessNode::%s]\tRegressor and Model created using %ld training points. Total time consumed: %ld nanoseconds.", __func__, cloud_gp->label.size(), elapsed );

    start = true;
    return true;
}

//
bool GaussianProcessNode::computeAtlas()
{
    //make sure we have a model and an object, we should have if start was called
    if (!object_ptr || object_ptr->empty()){
        ROS_ERROR("[GaussianProcessNode::%s]\tNo object initialized, call start service.",__func__);
        return false;
    }
    if (!obj_gp){
        ROS_ERROR("[GaussianProcessNode::%s]\tNo GP model initialized, call start service.",__func__);
        return false;
    }

    //right now just create N discs at random all depth 0.
    int N = 20;

    markers = boost::make_shared<visualization_msgs::MarkerArray>();
    fakeDeterministicSampling();
    int num_points = markers->markers[0].points.size();

    // ToCheck: I think this is only needed once, at computeGP() for instance
    gp_regression::ThinPlate my_kernel(R_);
    reg_.setCovFunction(my_kernel);

    gp_regression::GPProjector<gp_regression::ThinPlate> proj;
    for (int i=0; i < N; ++i)
    {
        int r_id;
        //get a random index
        r_id = getRandIn(0, num_points -1);
        gp_regression::Chart::Ptr gp_chart;
        Eigen::Vector3d c (markers->markers[0].points[r_id].x,
                           markers->markers[0].points[r_id].y,
                           markers->markers[0].points[r_id].z);
        // the size of the chart is equal to the variance at the center
        proj.generateChart(reg_, obj_gp, c, gp_chart);
        gp_chart->id = i;
        atlas_.addChart(gp_chart, i);
    }

/*
        //pcl
        //compute a normal around the point neighborhood (2cm)
        // pcl::search::KdTree<pcl::PointXYZRGB> kdtree;
        // std::vector<int> idx(object_ptr->points.size());
        // std::vector<float> dist(object_ptr->points.size());
        // kdtree.setInputCloud(object_ptr);
        // pcl::PointXYZRGB pt;
        // pt.x = points[i].x;
        // pt.y = points[i].y;
        // pt.z = points[i].z;
        // kdtree.radiusSearch(pt, 0.05, idx, dist);
        // pcl::NormalEstimationOMP<pcl::PointXYZRGB, pcl::Normal> ne_point;
        // ne_point.setInputCloud(object_ptr);
        // ne_point.useSensorOriginAsViewPoint();
        // float curv,nx,ny,nz;
        // ne_point.computePointNormal (*object_ptr, idx, nx,ny,nz, curv);
        // Eigen::Vector3d pclN;
        // pclN[0] = nx;
        // pclN[1] = ny;
        // pclN[2] = nz;
        // pclN.normalize();
        // //find a new x as close as possible to x kinect but orthonormal to N
        // X = kinX - (pclN*(pclN.dot(kinX)));
        // X.normalize();
        // Y = pclN.cross(X);
        // Y.normalize();
        // chart.Tx = X;
        // chart.Ty = Y;
        // //define a radius of the chart just take 3cm for now
        // chart.radius = 0.03f;
        // chart.id = r_id; //lets have the id = pointcloud id
        // chart.parent = 1; //doesnt have a parent since its root
        // atlas->insert(std::pair<uint8_t, Chart>(1,chart));
        // //
        // std::cout<<"NormalError: ";
        // double e;
        // e = sqrt( (pclN[0] - chart.N[0])*(pclN[0]-chart.N[0]) +
        //           (pclN[1] - chart.N[1])*(pclN[1]-chart.N[1]) +
        //           (pclN[2] - chart.N[2])*(pclN[2]-chart.N[2]) );
        // std::cout<<e<<std::endl;
        // std::cout<<"Error from pcl normal old lib: ";
        // e = sqrt( (chart.N[0]-gp_chart->N[0])*(chart.N[0]-gp_chart->N[0]) +
        //           (chart.N[1]-gp_chart->N[1])*(chart.N[1]-gp_chart->N[1]) +
        //           (chart.N[2]-gp_chart->N[2])*(chart.N[2]-gp_chart->N[2]) );
        // std::cout<<e<<std::endl;
*/
    isAtlas = true;
    createAtlasMarkers();
    return true;
}

//update gaussian model with new points from probe
/*
void GaussianProcessNode::update(Vec3Seq &points)
{
    Vec t;
    t.resize(points.size());
    for (auto &x : t)
        x=0;
    gp->add_patterns(points,t);
    //TODO temp we dont have an updated atlas, just recreate it from scratch
    computeAtlas();
}
*/

// Publish cloud method
void GaussianProcessNode::publishCloudModel () const
{
    // These checks are  to make sure we are not publishing empty cloud,
    // we have a  gaussian process computed and there's actually someone
    // who listens to us
    if (start && object_ptr)
        if(!model_ptr->empty() && pub_model.getNumSubscribers()>0)
            pub_model.publish(*model_ptr);
}

// for visualization purposes
void GaussianProcessNode::fakeDeterministicSampling()
{
    if(!markers)
        return;

    visualization_msgs::Marker sample;
    sample.header.frame_id = object_ptr->header.frame_id;
    sample.header.stamp = ros::Time::now();
    sample.lifetime = ros::Duration(0);
    sample.ns = "samples";
    sample.id = 0;
    sample.type = visualization_msgs::Marker::SPHERE_LIST;
    sample.action = visualization_msgs::Marker::ADD;
    sample.scale.x = 0.001;
    sample.scale.y = 0.001;
    sample.scale.z = 0.001;
    sample.color.a = 0.7;
    sample.color.r = 0.0;
    sample.color.b = 0.0;
    sample.color.g = 1.0;
    pcl::PointXYZRGB min, max;
    pcl::getMinMax3D(*object_ptr, min, max);
    double xm,xM,ym,yM,zm,zM;
    const double scale = 1.2; // ToDo: even in debug, pass this as param
    const double pass = 0.005; // ToDo: even in debug, pass this as param
    xm = ((1-scale)*max.x + (1+scale)*min.x)*0.5;
    ym = ((1-scale)*max.y + (1+scale)*min.y)*0.5;
    zm = ((1-scale*1.5)*max.z + (1+scale*1.5)*min.z)*0.5;
    xM = ((1+scale)*max.x + (1-scale)*min.x)*0.5;
    yM = ((1+scale)*max.y + (1-scale)*min.y)*0.5;
    zM = ((1+scale*1.5)*max.z + (1-scale*1.5)*min.z)*0.5;

    gp_regression::Data::Ptr qq = std::make_shared<gp_regression::Data>();
    gp_regression::ThinPlate my_kernel(R_);
    reg_.setCovFunction(my_kernel);

    for (double x = xm; x<= xM; x += pass) {
        for (double y = ym; y<= yM; y += pass) {
            for (double z = zm; z<= zM; z += pass) {

                qq->coord_x.clear();
                qq->coord_y.clear();
                qq->coord_z.clear();
                qq->coord_x.push_back(x);
                qq->coord_y.push_back(y);
                qq->coord_z.push_back(z);

                std::vector<double> ff;
                reg_.evaluate(obj_gp, qq, ff);

                // there will be only one test point at a time
                // so it is safe to evaluate at 0.
                if (ff.at(0) <= 0.001 && ff.at(0) >= -0.001) {
                    geometry_msgs::Point pt;
                    pt.x = x;
                    pt.y = y;
                    pt.z = z;
                    sample.points.push_back(pt);
                }
            }
        }
    }
    markers->markers.push_back(sample);
}

void GaussianProcessNode::createAtlasMarkers()
{
    if (!isAtlas){
        ROS_WARN("[GaussianProcessNode::%s]\tNo Atlas created, not computing any marker.",__func__);
        return ;
    }

    // Now show the Atlas
    // for each atlas (we have 1 now TODO loop)
    // int a (0); //atlas index
    // {
        //for each chart
        for(int i = 0; i < atlas_.charts_.size(); ++i)
        {
            visualization_msgs::Marker disc;
            disc.header.frame_id = object_ptr->header.frame_id;
            disc.header.stamp = ros::Time::now();
            disc.lifetime = ros::Duration(0);
            disc.frame_locked = true;
            std::string ns("A" + std::to_string(0) + "_D" + std::to_string(i));
            disc.ns = ns;
            disc.id = atlas_.charts_.at(i).id + cb_rnd_choose_counter;
            disc.type = visualization_msgs::Marker::CYLINDER;
            disc.action = visualization_msgs::Marker::ADD;
            disc.scale.x = 0.03; // atlas_.charts_.at(i).R;
            disc.scale.y = 0.03; // atlas_.charts_.at(i).R;
            disc.scale.z = 0.001;
            disc.color.a = 0.5;
            disc.color.r = 0.8;
            disc.color.b = 1.0;
            disc.color.g = 0.0;
            Eigen::Matrix3d rot;
            rot.col(0) =  atlas_.charts_.at(i).Tx;
            rot.col(1) =  atlas_.charts_.at(i).Ty;
            rot.col(2) =  atlas_.charts_.at(i).N;
            Eigen::Quaterniond q(rot);
            q.normalize();
            disc.pose.orientation.x = q.x();
            disc.pose.orientation.y = q.y();
            disc.pose.orientation.z = q.z();
            disc.pose.orientation.w = q.w();
            disc.pose.position.x = atlas_.charts_.at(i).C(0);
            disc.pose.position.y = atlas_.charts_.at(i).C(1);
            disc.pose.position.z = atlas_.charts_.at(i).C(2);
            markers->markers.push_back(disc);

            geometry_msgs::Point end;
            geometry_msgs::Point start;

            visualization_msgs::Marker aZ;
            aZ.header.frame_id = object_ptr->header.frame_id;
            aZ.header.stamp = ros::Time::now();
            aZ.lifetime = ros::Duration(0);
            aZ.frame_locked = true;
            std::string nsa("N" + std::to_string(atlas_.charts_.at(i).id));
            aZ.ns = nsa;
            aZ.id = 0 + cb_rnd_choose_counter;
            aZ.type = visualization_msgs::Marker::ARROW;
            aZ.action = visualization_msgs::Marker::ADD;
            start.x = atlas_.charts_.at(i).C(0);
            start.y = atlas_.charts_.at(i).C(1);
            start.z = atlas_.charts_.at(i).C(2);
            aZ.points.push_back(start);
            end.x = start.x + atlas_.charts_.at(i).N[0]/10;
            end.y = start.y + atlas_.charts_.at(i).N[1]/10;
            end.z = start.z + atlas_.charts_.at(i).N[2]/10;
            aZ.points.push_back(end);
            aZ.scale.x = 0.002;
            aZ.scale.y = 0.008;
            aZ.scale.z = 0.008;
            aZ.color.a = 0.5;
            aZ.color.r = aZ.color.g = 0.0;
            aZ.color.b = 1.0;
            markers->markers.push_back(aZ);
/*
            visualization_msgs::Marker aX;
            aX.header.frame_id = object_ptr->header.frame_id;
            aX.header.stamp = ros::Time::now();
            aX.lifetime = ros::Duration(0);
            std::string nsx("Tx" + std::to_string(atlas_.charts_.at(i).id));
            aX.ns = nsx;
            aX.id = 0;
            aX.type = visualization_msgs::Marker::ARROW;
            aX.action = visualization_msgs::Marker::ADD;
            start.x = atlas_.charts_.at(i).C(0);
            start.y = atlas_.charts_.at(i).C(1);
            start.z = atlas_.charts_.at(i).C(2);
            aX.points.push_back(start);
            end.x = start.x + atlas_.charts_.at(i).Tx[0]/10;
            end.y = start.y + atlas_.charts_.at(i).Tx[1]/10;
            end.z = start.z + atlas_.charts_.at(i).Tx[2]/10;
            aX.points.push_back(end);
            aX.scale.x = 0.002;
            aX.scale.y = 0.008;
            aX.scale.z = 0.008;
            aX.color.a = 0.5;
            aX.color.b = aX.color.g = 0.0;
            aX.color.r = 1.0;
            markers->markers.push_back(aX);

            visualization_msgs::Marker aY;
            aY.header.frame_id = object_ptr->header.frame_id;
            aY.header.stamp = ros::Time::now();
            aY.lifetime = ros::Duration(0);
            std::string nsy("Ty" + std::to_string(atlas_.charts_.at(i).id));
            aY.ns = nsy;
            aY.id = 0;
            aY.type = visualization_msgs::Marker::ARROW;
            aY.action = visualization_msgs::Marker::ADD;
            start.x = atlas_.charts_.at(i).C(0);
            start.y = atlas_.charts_.at(i).C(1);
            start.z = atlas_.charts_.at(i).C(2);
            aY.points.push_back(start);
            end.x = start.x + atlas_.charts_.at(i).Ty[0]/10;
            end.y = start.y + atlas_.charts_.at(i).Ty[1]/10;
            end.z = start.z + atlas_.charts_.at(i).Ty[2]/10;
            aY.points.push_back(end);
            aY.scale.x = 0.002;
            aY.scale.y = 0.008;
            aY.scale.z = 0.008;
            aY.color.a = 0.5;
            aY.color.b = aY.color.r = 0.0;
            aY.color.g = 1.0;
            markers->markers.push_back(aY);
*/
    }
        ROS_INFO("ATLAS MARKERS GENERATED");
}

//Publish sample (remove for now, we are publishing atlas markers)
void GaussianProcessNode::publishAtlas () const
{
    if (markers)
        if(pub_markers.getNumSubscribers() > 0)
            pub_markers.publish(*markers);
}

// test for occlusion of samples (now unused)
// return:
//  0 -> not visible
//  1 -> visible
//  -1 -> error
// int GaussianProcessNode::isSampleVisible(const pcl::PointXYZRGB sample, const float min_z) const
// {
//     if(!viewpoint_tree){
//         ROS_ERROR("[GaussianProcessNode::%s]\tObject Viewpoint KdTree is not initialized. Aborting...",__func__);
//         //should never happen if called from sampleAndPublish
//         return (-1);
//     }
//     Eigen::Vector3f camera(0,0,0);
//     Eigen::Vector3f start_point(sample.x, sample.y, sample.z);
//     Eigen::Vector3f direction = camera - start_point;
//     const float norm = direction.norm();
//     direction.normalize();
//     const float step_size = 0.01f;
//     const int nsteps = std::max(1, static_cast<int>(norm/step_size));
//     std::vector<int> k_id;
//     std::vector<float> k_dist;
//     //move along direction
//     Eigen::Vector3f p(start_point[0], start_point[1], start_point[2]);
//     for (size_t i = 0; i<nsteps; ++i)
//     {
//         if (p[2] <= min_z)
//             //don't reach  the sensor, if  we are  outside sample region  we can
//             //stop testing.
//             break;
//         pcl::PointXYZRGB pt;
//         pt.x = p[0];
//         pt.y = p[1];
//         pt.z = p[2];
//         // TODO: This search radius is  hardcoded now, should be adapted somehow
//         // on point density (tabjones on Friday 13/11/2015)
//         if (viewpoint_tree->radiusSearch(pt, 0.005, k_id, k_dist, 1) > 0)
//             //we intersected an object point, this sample cant reach the camera
//             return(0);
//         p += (direction * step_size);
//     }
//     //we didn't intersect anything, this sample is not occluded
//     return(1);
// }
