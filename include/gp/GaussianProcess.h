/** @file GaussianProcess.h
 *
 *
 * @author  Claudio Zito
 *
 * @copyright  Copyright (C) 2015 Claudio, University of Birmingham, UK
 *
 * @license  This file copy is licensed to you under the terms described in
 *           the License.txt file included in this distribution.
 *
 * Refer to Gaussian process library for Machine Learning.
 *
 */
#ifndef __GP_GAUSSIANPROCESS_H__
#define __GP_GAUSSIANPROCESS_H__
#define _USE_MATH_DEFINES

//------------------------------------------------------------------------------

#include <cstdio>
#include <cmath>
#include "gp/SampleSet.h"
#include "gp/CovLaplace.h"
#include "gp/CovThinPlate.h"
#include "gp/CovSE.h"
// #include <omp.h>

//------------------------------------------------------------------------------

namespace gp {

//------------------------------------------------------------------------------

/** Utily constants */
static const double log2pi = std::log(numeric_const<double>::TWO_PI);
template <class CovTypePtr, class CovTypeDesc> class GaussianProcess;

//------------------------------------------------------------------------------

/** 
* Gradient-based optimizer (RProp).
*
*/
class Optimisation {
public:
    /** Pointer */
    typedef boost::shared_ptr<Optimisation> Ptr;

    /** Description file */
    class Desc {
    public:
        /** Pointer to the descriptor */
        typedef boost::shared_ptr<Desc> Ptr;

        double delta0;
        double deltaMin;
        double deltaMax;
        double etaMinus;
        double etaPlus;
        double epsStop;
        size_t maxIter;

        Desc() {
            setToDefault();
        }
        void setToDefault() {
            delta0 = 0.1;
            deltaMin = 1e-6;
            deltaMax = 50;
            etaMinus = 0.5;
            etaPlus = 1.2;
            epsStop = 1e-4;

            maxIter = 100;
        }
        bool isValid() {
            return true;
        }
        /** Creates the object from the description. */
        Optimisation::Ptr create() const {
            Optimisation::Ptr pointer(new Optimisation);
            pointer->create(*this);
            return pointer;
        }
    };

    template <typename CovTypePtr, typename CovTypeDesc> void find(GaussianProcess<CovTypePtr, CovTypeDesc>* gp, bool verbose = true) {
        clock_t t = clock();
        int paramDim = gp->cf->getParamDim();
        Eigen::VectorXd delta = Eigen::VectorXd::Ones(paramDim) * delta0;
        Eigen::VectorXd gradOld = Eigen::VectorXd::Zero(paramDim);
        Eigen::VectorXd params = gp->cf->getLogHyper();
        Eigen::VectorXd bestParams = params;
        double best = log(0);

        printf("Optimisation::find(): iter=0 params[l,s]=[%f, %f]\n", params(0), params(1));
        for (size_t i = 0; i < maxIter; ++i) {
            Eigen::VectorXd grad = -gp->logLikelihoodGradient();
            gradOld = gradOld.cwiseProduct(grad);
            for (int j = 0; j < gradOld.size(); ++j) {
                if (gradOld(j) > 0) {
                    delta(j) = std::min(delta(j)*etaPlus, deltaMax);
                }
                else if (gradOld(j) < 0) {
                    delta(j) = std::max(delta(j)*etaMinus, deltaMin);
                    grad(j) = 0;
                }
                params(j) += -sign(grad(j)) * delta(j);
            }
            gradOld = grad;
            if (gradOld.norm() < epsStop) break;
            gp->cf->setLogHyper(params);
            double lik = gp->logLikelihood();
            if (verbose)
                printf("Optimisation::find(): iter=%lu lik=%f params[l,s]=[%f, %f]\n", i+1, lik, params(0), params(1));
            //std::cout << i << " " << -lik << std::endl;
            if (lik > best) {
                best = lik;
                bestParams = params;
            }
        }
        gp->cf->setLogHyper(bestParams);
        printf("Optimisation::find(): best lik=%f params[l,s]=[%f, %f]\nElapsed time: %.4fs\n", best, bestParams(0), bestParams(1), (float)(clock() - t) / CLOCKS_PER_SEC);
    }
    /** D'ctor */
    //  ~Optimisation() {} // don't need to do anything

protected:
    /** Optimisation parameters */
    double delta0;
    double deltaMin;
    double deltaMax;
    double etaMinus;
    double etaPlus;
    double epsStop;

    size_t maxIter;

    double sign(double x) {
        if (x > 0) return 1.0;
        if (x < 0) return -1.0;
        return 0.0;
    }

    /** Create from descriptor */
    void create(const Desc& desc) {
        delta0 = desc.delta0;
        deltaMin = desc.deltaMin;
        deltaMax = desc.deltaMax;
        etaMinus = desc.etaMinus;
        etaPlus = desc.etaPlus;
        epsStop = desc.epsStop;

        maxIter = desc.maxIter;
    };
    /** Default C'ctor */
    Optimisation() {}
};

//------------------------------------------------------------------------------

/*
* Gaussian Process.
*
*/
template <class CovTypePtr, class CovTypeDesc> class GaussianProcess {
public:
    /** Needed for good alignment for the Eigen's' data structures */
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW
    /** Pointer to the class */
    typedef boost::shared_ptr<GaussianProcess<CovTypePtr, CovTypeDesc> > Ptr;
    friend class Optimisation;

    /** Descriptor file */
    class Desc {
        public:
            /** Initial size of the kernel matrix */
            size_t initialLSize;
            /** Noise use to compute K(x,x) */
            double noise;
            /** Covariance description file */
            CovTypeDesc covTypeDesc;

            /** Enable optimisation */
            bool optimise;
            /** Optimisation procedure descriptor file */
            Optimisation::Desc::Ptr optimisationDescPtr;
            /** Enable atlas */
            bool atlas;

            /** C'tor */
            Desc() {
                setToDefault();
            }

            /** Set to default */
            void setToDefault() {
                initialLSize = 1500;
                noise = numeric_const<double>::ZERO;
                covTypeDesc.setToDefault();
                
                optimise = false;

                optimisationDescPtr.reset(new Optimisation::Desc);

                atlas = false;
            }

            /** Creates the object from the description. */
            Ptr create() const {
                Ptr pointer(new GaussianProcess<CovTypePtr, CovTypeDesc>);
                pointer->create(*this);
                return pointer;
            }

            /** Assert valid descriptor files */
            bool isValid() {
                if (!std::isfinite(initialLSize))
                    return false;
                if (noise < numeric_const<double>::ZERO)
                    return false;
                return true;
            }
    };

    /** Predict f_* ~ GP(x_*) */
    void evaluate(const Vec3Seq& x, std::vector<Real>& fx, std::vector<Real>& vars) {
        const size_t size = x.size();
        fx.assign(size, 0.0);
        vars.assign(size, 0.0);
        // for (size_t i = 0; i < size; ++i) {
        //      fx[i] = f(x[i]);
        //      vars[i] = var(x[i]);
        // }
    }

    /** Predict f_* ~ GP(x_*) */
    virtual Eigen::Vector4d f(const Vec3& xStar) {
        if (sampleset->empty()) 
            throw std::invalid_argument("No training data available");

        // Eigen::Map<const Eigen::VectorXd> x_star(x.v, input_dim);
        compute(true);
        update_alpha();
        update_k_star(xStar);
        // std::cout << "size alpha=" << alpha->size() << " k_star=" << k_star->size() << std::endl;
        Eigen::Vector4d sol;
        for (size_t i = 0; i < 4; ++i) {
            sol(i) = k_star->row(i).dot(*alpha);
        }
        return sol;
    }


    /** Predict variance v[f_*] ~ var(x_*)  */
    virtual Eigen::Matrix4d var(const Vec3& xStar) {

        if (sampleset->empty())
            return 0;

        // Eigen::Map<const Eigen::VectorXd> x_star(x.v, input_dim);
        compute(true);
        update_alpha();
        update_k_star(xStar); //update_k_star(x_star);
        size_t n = sampleset->rows();
        Eigen::VectorXd ks = k_star->block(0, 0, 1, n);
        Eigen::VectorXd v = L->topLeftCorner(n, n).triangularView<Eigen::Lower>().solve(ks);
        return cf->get(xStar, xStar) - v.dot(v); //cf->get(x_star, x_star) - v.dot(v);
    }

    // virtual void evaluate(const Vec3& x, Real& fx, Real& varx, Eigen::Vector3d& normal, Eigen::Vector3d& tx, Eigen::Vector3d& ty) {
    // }
    // virtual void evaluate(Eigen::MatrixXd& normal, Eigen::MatrixXd& tx, Eigen::MatrixXd& ty) {
    // }

    /** Predict f, var, N, Tx and Ty */
    virtual void evaluate(const Vec3Seq& x, std::vector<Real>& fx, std::vector<Real>& varx, Eigen::MatrixXd& normals, Eigen::MatrixXd& tx, Eigen::MatrixXd& ty) {
        clock_t t = clock();

        // compute f(x) and V(x)
        evaluate(x, fx, varx);

        const size_t np = sampleset->rows(), nq = x.size(), n = sampleset->rows() + x.size();
        const size_t dim = sampleset->cols();

        // differential of covariance with selected kernel
        Eigen::MatrixXd Kqp, Kqpdiff;
        // row = number of query points
        // col = number of points in the kernels + 1
        Kqp.resize(nq, np + 1);
        Kqpdiff.resizeLike(Kqp);
        normals.resize(nq, dim);
        tx.resize(nq, dim);
        ty.resize(nq, dim);

        // compute Kqp and KqpDiff
        for (size_t i = 0; i < nq; ++i) {
            for (size_t j = 0; j < np; ++j) {
                Kqp(i, j) = cf->get(x[i], sampleset->x(j));
                //Kqpdiff(i, j) = cf->getDiff(x[i], sampleset->x(j));
            }
        }
        // compute error
        Real error = 0.0;
        //#pragma omp parallel for
        for (size_t i = 0; i < nq; ++i) {
            for (size_t j = 0; j < np; ++j) {
                normals.row(i) += InvKppY(j)*Kqpdiff(i, j)*(convertToEigen(x[i] - sampleset->x(j)));
            }

            normals.row(i).normalize();
            Vec3 ni(normals(i, 0), normals(i, 1), normals(i, 2)), p = x[i];
            p.normalise();
            error += p.distance(ni);
            // context.write("N[%d] = [%f %f %f]\n", i, N(i, 0), N(i, 1), N(i, 2));
            Eigen::Vector3d Ni = normals.row(i);
            Eigen::Vector3d Txi, Tyi;
            computeTangentBasis(Ni, Txi, Tyi);
            tx.row(i) = Txi;
            ty.row(i) = Tyi;
        }
        //printf("GP::evaluate(): Error=%lu\nElapsed time: %.4fs\n", error / nq, (float)(clock() - t) / CLOCKS_PER_SEC);
    }


    /** Set training data */
    void set(SampleSet::Ptr trainingData) {
        sampleset = trainingData;
        // param optimisation
        if (desc.optimise)
            optimisationPtr->find<CovTypePtr, CovTypeDesc>(this);
    }

    /** Get name of the covariance function */
    std::string getName() const {
        return cf->getName();
    }

    /** Add input-output pairs to sample set. */
    void add_patterns(const Vec3Seq& newInputs, const RealSeq& newTargets) {
        assert(newInputs.size() == newTargets.size());

        // the size of the inputs before adding new samples
        const size_t n = sampleset->rows();
        sampleset->add(newInputs, newTargets, newInputs);

        // create kernel matrix if sampleset is empty
        if (n == 0) {
            cf->setLogHyper(true);
            compute();
        }
        else {
            clock_t t = clock();
            const size_t nnew = sampleset->rows();
            // resize L if necessary
            if (sampleset->rows() > static_cast<std::size_t>(L->rows())) {
                L->conservativeResize(nnew + initialLSize, nnew + initialLSize);
            }
            //#pragma omp parallel for
            for (size_t j = n; j < nnew; ++j) {
                Eigen::VectorXd k(j);
                for (size_t i = 0; i<j; ++i) {
                    k(i) = cf->get(sampleset->x(i), sampleset->x(j));
                    //printf("GP::add_patters(): Computing k(%lu, %lu)\r", i, j);
                }
                const double kappa = cf->get(sampleset->x(j), sampleset->x(j));
                L->topLeftCorner(j, j).triangularView<Eigen::Lower>().solveInPlace(k);
                L->block(j,0,1,j) = k.transpose();
                (*L)(j,j) = sqrt(kappa - k.dot(k));
            }
            printf("GP::add_patterns(): Elapsed time: %.4fs\n", (float)(clock() - t)/CLOCKS_PER_SEC);
        }
        alpha_needs_update = true;
    }

    /** Compute loglikelihood */
    double logLikelihood() {
        compute();
        update_alpha();
        int n = sampleset->rows();
        const std::vector<double>& targets = sampleset->y();
        Eigen::Map<const Eigen::VectorXd> y(&targets[0], sampleset->rows());
        double det = (double)(2 * L->diagonal().head(n).array().log().sum());
        return (double)(- 0.5*y.dot(*alpha) - 0.5*det - 0.5*n*log2pi);
    }

    Eigen::VectorXd logLikelihoodGradient() {
        compute(false);
        update_alpha();
        size_t n = sampleset->rows();
        Eigen::VectorXd grad = Eigen::VectorXd::Zero(cf->getParamDim());
        Eigen::VectorXd g(grad.size());
        Eigen::MatrixXd W = Eigen::MatrixXd::Identity(n, n);

        // compute kernel matrix inverse
        L->topLeftCorner(n, n).triangularView<Eigen::Lower>().solveInPlace(W);
        L->topLeftCorner(n, n).triangularView<Eigen::Lower>().transpose().solveInPlace(W);

        W = (*alpha) * alpha->transpose() - W;

        for (size_t i = 0; i < n; ++i) {
            for (size_t j = 0; j <= i; ++j) {
                cf->grad(sampleset->x(i), sampleset->x(j), g);
                if (i == j) grad += W(i, j) * g * 0.5;
                else grad += W(i, j) * g;
            }
        }

        return grad;
    }

    inline Eigen::MatrixXd getNormals() const { return N; }

    /** D'tor */
    virtual ~GaussianProcess() {};

protected:

    /** Desriptor file */
    Desc desc;

    /** Optimisation procedure */
    Optimisation::Ptr optimisationPtr;

    /** pointer to the covariance function type */
    CovTypePtr cf;
    /** The training sample set. */
    boost::shared_ptr<SampleSet> sampleset;
    /** Alpha is cached for performance. */
    boost::shared_ptr<Eigen::VectorXd> alpha;
    /** Last test kernel vector. */
    boost::shared_ptr<Eigen::VectorXd> k_star;
    /** Linear solver used to invert the covariance matrix. */
    // Eigen::LLT<Eigen::MatrixXd> solver;
    boost::shared_ptr<Eigen::MatrixXd> L;
    /** Input vector dimensionality. */
    size_t input_dim;
    /** Noise parameter */
    Real delta_n;
    /** Enable/disable to update the alpha vector */
    bool alpha_needs_update;
    //initial L size
    size_t initialLSize;

    /** (inward) normal sequence */
    Eigen::MatrixXd N;
    /** Tangent basis in x direction */
    Eigen::MatrixXd Tx;
    /** Tangen basis in y direction */
    Eigen::MatrixXd Ty;
    /** Weights */
    Eigen::VectorXd InvKppY;
    /** Derivative of the kernel */
    Eigen::MatrixXd Kppdiff;

    /** Compute k_* = K(x_*, x) */
    void update_k_star(const Vec3&x_star) {
        // k_star->resize(sampleset->rows());
        // for(size_t i = 0; i < sampleset->rows(); ++i) {
        //      (*k_star)(i) = cf->get(x_star, sampleset->x(i));
        // }
        const size_t n = sampleset->rows();
        const size_t ndt = 4 * n;
        k_star->resize(4, ndt);
        // first row in K*
        // -> [x*x1, ... , x*xn, dx(x*x1), dy(x*x1), dz(x*x1), ..., dx(x*xn), dy(x*xn), dz(x*xn)] [1x4n]
        size_t i = 0;
        for (size_t j = 0; j < n; ++j) {
            (*k_star)(i, j) = cf->get(x_star, sampleset->x(j));
        }
        for (size_t j = 0; j < n; ++j) {
            for (size_t d = 0; d < 3; ++d) {
                (*k_star)(i, n + 3 * j + d) = cf->getDiff(x_star, sampleset->x(j), d);
            }
    }

        // 2nd row in K*
        // -> [dx(x*x1), dx(x*x2), dx(x*x3), ..., dx(x*xn), ...
        //    ... dxdx(x*x1), dxdy(x*x1), dxdz(x*x1), ..., dxdx(x*xn), dxdy(x*xn), dxdz(x*xn)] [1x4n]
        // the loop goes for 3rd and 4th row as well -> [3x4n]
        for (i = 1; i < 4; ++i) {
            size_t j = 0;
            for (j = 0; j < n; ++j) {
                (*k_star)(i, j) = cf->getDiff(x_star, sampleset->x(j), i-1);
            }
            for (; j < ndt; ++j) {
                for (size_t d = 0; d < 3; ++d) {
                    (*k_star)(i, j) = cf->getDiff(x_star, sampleset->x(j), i-1, d);
                }
            }

        }
        //printf("K_star [%d %d]\n", k_star->rows(), k_star->cols());
        //for (size_t i = 0; i < k_star->rows(); ++i) {
        //  for (size_t j = 0; j < k_star->cols(); ++j)
        //      printf("%f ", (*k_star)(i, j));
        //  context.write("\n");
        //}
        //for (size_t i = 0; i < 4; ++i) {
        //  (*k_star)(i, j) = cf->get(x_star, sampleset->x(i));
        //}

    }

    /** Update alpha vector (mean) */
    void update_alpha() {
         // can previously computed values be used?
        if (!alpha_needs_update) return;
        alpha_needs_update = false;
        const size_t ndt = 4 * sampleset->rows();
        alpha->resize(ndt);
        // Map target values to VectorXd
        //const std::vector<double>& targets = sampleset->y();
        const RealSeq& targets = sampleset->y();
        if (targets.size() != ndt)
            throw std::invalid_argument( "Target and alpha vector have a size dismatch" );

        Eigen::Map<const Eigen::VectorXd> y(&targets[0], ndt);
        *alpha = L->topLeftCorner(ndt, ndt).triangularView<Eigen::Lower>().solve(y);
        //printf("alpha [%d %d]\n", alpha->rows(), alpha->cols());
        //for (size_t i = 0; i < alpha->size(); ++i)
        //  printf("%f ", (*alpha)(i));
        //printf("\n");
        L->topLeftCorner(ndt, ndt).triangularView<Eigen::Lower>().adjoint().solveInPlace(*alpha);
        //printf("alpha2 [%d %d]\n", alpha->rows(), alpha->cols());
        //for (size_t i = 0; i < alpha->size(); ++i)
        //  printf("%f ", (*alpha)(i));
        //printf("\n");
   }

    /** Compute covariance matrix and perform cholesky decomposition. */
    virtual void compute(const bool verbose = false) {
         // can previously computed values be used?
        if (!cf->isLogHyper())
            return;
        clock_t t = clock();
        cf->setLogHyper(false);
        // input size
        const size_t n = sampleset->rows();
        const size_t dim = sampleset->cols();

    // resize L if necessary
        if (4 * n > L->rows()) 
            L->resize(4 * n + initialLSize, 4 * n + initialLSize);

    // compute kernel matrix (lower triangle)
//#pragma omp parallel for collapse(2)
        for(size_t i = 0; i < n; ++i) {
            for(size_t j = 0; j <= i; ++j) {
                // compute kernel and add noise on the diagonal of the kernel
                (*L)(i, j) = cf->get(sampleset->x(i), sampleset->x(j), i == j);
        //      context.write("Computing L(%lu, %lu) = %f\n", i, j, (*L)(i, j));
            }
        }   
        // first derivative
        for (size_t i = 0; i < n; ++i) {
            for (size_t d = 0; d < 3; ++d) {
                for (size_t j = 0; j < n; ++j) {
                    // compute kernel and add noise on the diagonal of the kernel
                    (*L)(n + i * 3 + d, j) = cf->getDiff(sampleset->x(i), sampleset->x(j), d, i == j);
    //                  context.write("Computing L(%lu, %lu)=d%luk(%lu, %lu) = %f\n", n + i * 3 + d, j, d, i, j, (*L)(n + i * 3 + d, j));
                }
                for (size_t j = 0; j < i * 3 + d + 1; ++j) {
                    (*L)(n + i * 3 + d, n + j) = cf->getDiff2(sampleset->x(i), sampleset->x(j), d, j % 3, i == j);
//                  context.write("Computing L(%lu, %lu)=d%lud%luk(%lu, %lu) = %f\n", n + i * 3 + d, n + j, d, j % 3, i, j, (*L)(n + i * 3 + d, n + j));
                }
            }
        }
        const size_t ndt = 4 * n;
        InvKppY = L->topLeftCorner(ndt, ndt).inverse() * convertToEigen(sampleset->y());
        //context.write("L: [%d %d]\n", ndt, ndt);
        //for (size_t i = 0; i < ndt; ++i) {
        //  for (size_t j = 0; j <= i; ++j) {
        //      context.write("%.3f ", (*L)(i, j));
        //  }
        //  context.write("\n");
        //}
        // perform cholesky factorization
        L->topLeftCorner(ndt, ndt) = L->topLeftCorner(ndt, ndt).selfadjointView<Eigen::Lower>().llt().matrixL();
        alpha_needs_update = true;
        if (verbose)
            printf("GP::Compute(): Elapsed time: %.4fs\n", (float)(clock() - t)/CLOCKS_PER_SEC);
    }

    /** Compute tangent basis. N must be normalised */
    void computeTangentBasis(const Eigen::Vector3d &N, Eigen::Vector3d &Tx, Eigen::Vector3d &Ty) {
        Eigen::Matrix3d TProj = Eigen::Matrix3d::Identity() - N*N.transpose();
        Eigen::JacobiSVD<Eigen::Matrix3d> svd(TProj, Eigen::ComputeFullU | Eigen::ComputeFullV);
        Tx = svd.matrixU().col(0);
        Ty = svd.matrixU().col(1);
    }

    /** Create from description file */
    void create(const Desc& desc) {
        this->desc = desc;

        optimisationPtr = desc.optimisationDescPtr->create();

        cf = desc.covTypeDesc.create();
        //sampleset = desc.trainingData;
        input_dim = 3;
        delta_n = desc.noise;
        initialLSize = desc.initialLSize;
        alpha.reset(new Eigen::VectorXd);
        k_star.reset(new Eigen::VectorXd);
        L.reset(new Eigen::MatrixXd);
        L->resize(initialLSize, initialLSize);
        alpha_needs_update = true;
    }

    /** Default C'tor */
    GaussianProcess() {}
};

//------------------------------------------------------------------------------

/** List of legal types */
typedef GaussianProcess<gp::BaseCovFunc::Ptr, gp::Laplace::Desc> LaplaceRegressor;
typedef GaussianProcess<gp::BaseCovFunc::Ptr, gp::CovSE::Desc> GaussianRegressor;
typedef GaussianProcess<gp::BaseCovFunc::Ptr, gp::CovSEArd::Desc> GaussianARDRegressor;
typedef GaussianProcess<gp::BaseCovFunc::Ptr, gp::ThinPlate::Desc> ThinPlateRegressor;

//typedef GaussianProcess<gp::Laplace::Ptr, gp::Laplace::Desc> LaplaceRegressor;
//typedef GaussianProcess<gp::ThinPlate> ThinPlateRegressor;
//class LaplaceRegressor : public GaussianProcess<gp::Laplace> {};

//------------------------------------------------------------------------------

}
#endif /* __GP_GAUSSIANPROCESS_H__ */

