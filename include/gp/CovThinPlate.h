#ifndef __GP__THINPLATE_H__
#define __GP__THINPLATE_H__

//------------------------------------------------------------------------------

#include "gp/Covs.h"

//------------------------------------------------------------------------------

namespace gp
{

//------------------------------------------------------------------------------

class ThinPlate : public BaseCovFunc {
public:
        const double length_;
        
        //thin plate kernel = 2.*EE.^3 - 3.*(leng).* EE.^2 + (leng*ones(size(EE))).^3
        double get(const Vec3& x1, const Vec3& x2) const {
//        	const double sum_x1_2 = x1.magnitudeSqr();
//        	const double sum_x2_2 = x2.magnitudeSqr();
//        	const double DD = sum_x1_2 + sum_x2_2 - 2*x1.dot(x2);
        	const double EE = x1.distance(x2);//sqrt(DD);
        	return 2*std::pow(EE, 3.0) - 3*length_*pow(EE, 2.0) + length_;
        }

        ThinPlate(const double length) : BaseCovFunc(), length_(length) {
                length_3 = std::pow(length_, 3.0);
                loghyper_changed = true;
        }

        ThinPlate() : BaseCovFunc(), length_(1.0)
        {
                length_3 = std::pow(length_, 3.0);
                loghyper_changed = true;
        }

private:
        double length_3;
};

//------------------------------------------------------------------------------

}

#endif
