/*
 * @file EssentialMatrix.cpp
 * @brief EssentialMatrix class
 * @author Frank Dellaert
 * @date December 5, 2014
 */

#include <gtsam/geometry/EssentialMatrix.h>
#include <iostream>

namespace gtsam {

/* ************************************************************************* */
EssentialMatrix EssentialMatrix::FromPose3(const Pose3& _1P2_,
    boost::optional<Matrix&> H) {
  const Rot3& _1R2_ = _1P2_.rotation();
  const Point3& _1T2_ = _1P2_.translation();
  if (!H) {
    // just make a direction out of translation and create E
    Sphere2 direction(_1T2_);
    return EssentialMatrix(_1R2_, direction);
  } else {
    // Calculate the 5*6 Jacobian H = D_E_1P2
    // D_E_1P2 = [D_E_1R2 D_E_1T2], 5*3 wrpt rotation, 5*3 wrpt translation
    // First get 2*3 derivative from Sphere2::FromPoint3
    Matrix D_direction_1T2;
    Sphere2 direction = Sphere2::FromPoint3(_1T2_, D_direction_1T2);
    H->resize(5, 6);
    H->block<3, 3>(0, 0) << Matrix::Identity(3, 3); // upper left
    H->block<2, 3>(3, 0) << Matrix::Zero(2, 3); // lower left
    H->block<3, 3>(0, 3) << Matrix::Zero(3, 3); // upper right
    H->block<2, 3>(3, 3) << D_direction_1T2 * _1R2_.matrix(); // lower right
    return EssentialMatrix(_1R2_, direction);
  }
}

/* ************************************************************************* */
void EssentialMatrix::print(const std::string& s) const {
  std::cout << s;
  aRb_.print("R:\n");
  aTb_.print("d: ");
}

/* ************************************************************************* */
EssentialMatrix EssentialMatrix::retract(const Vector& xi) const {
  assert(xi.size() == 5);
  Vector3 omega(sub(xi, 0, 3));
  Vector2 z(sub(xi, 3, 5));
  Rot3 R = aRb_.retract(omega);
  Sphere2 t = aTb_.retract(z);
  return EssentialMatrix(R, t);
}

/* ************************************************************************* */
Vector EssentialMatrix::localCoordinates(const EssentialMatrix& other) const {
  return Vector(5) << //
      aRb_.localCoordinates(other.aRb_), aTb_.localCoordinates(other.aTb_);
}

/* ************************************************************************* */
Point3 EssentialMatrix::transform_to(const Point3& p,
    boost::optional<Matrix&> DE, boost::optional<Matrix&> Dpoint) const {
  Pose3 pose(aRb_, aTb_.point3());
  Point3 q = pose.transform_to(p, DE, Dpoint);
  if (DE) {
    // DE returned by pose.transform_to is 3*6, but we need it to be 3*5
    // The last 3 columns are derivative with respect to change in translation
    // The derivative of translation with respect to a 2D sphere delta is 3*2 aTb_.basis()
    // Duy made an educated guess that this needs to be rotated to the local frame
    Matrix H(3, 5);
    H << DE->block<3, 3>(0, 0), -aRb_.transpose() * aTb_.basis();
    *DE = H;
  }
  return q;
}

/* ************************************************************************* */
EssentialMatrix EssentialMatrix::rotate(const Rot3& cRb,
    boost::optional<Matrix&> HE, boost::optional<Matrix&> HR) const {

  // The rotation must be conjugated to act in the camera frame
  Rot3 c1Rc2 = aRb_.conjugate(cRb);

  if (!HE && !HR) {
    // Rotate translation direction and return
    Sphere2 c1Tc2 = cRb * aTb_;
    return EssentialMatrix(c1Rc2, c1Tc2);
  } else {
    // Calculate derivatives
    Matrix D_c1Tc2_cRb, D_c1Tc2_aTb; // 2*3 and 2*2
    Sphere2 c1Tc2 = cRb.rotate(aTb_, D_c1Tc2_cRb, D_c1Tc2_aTb);
    if (HE) {
      *HE = zeros(5, 5);
      HE->block<3, 3>(0, 0) << cRb.matrix(); // a change in aRb_ will yield a rotated change in c1Rc2
      HE->block<2, 2>(3, 3) << D_c1Tc2_aTb; // (2*2)
    }
    if (HR) {
      throw std::runtime_error(
          "EssentialMatrix::rotate: derivative HR not implemented yet");
      /*
       HR->resize(5, 3);
       HR->block<3, 3>(0, 0) << zeros(3, 3); // a change in the rotation yields ?
       HR->block<2, 3>(3, 0) << zeros(2, 3); // (2*3) * (3*3) ?
       */
    }
    return EssentialMatrix(c1Rc2, c1Tc2);
  }
}

/* ************************************************************************* */
double EssentialMatrix::error(const Vector& vA, const Vector& vB, //
    boost::optional<Matrix&> H) const {
  if (H) {
    H->resize(1, 5);
    // See math.lyx
    Matrix HR = vA.transpose() * E_ * skewSymmetric(-vB);
    Matrix HD = vA.transpose() * skewSymmetric(-aRb_.matrix() * vB)
        * aTb_.basis();
    *H << HR, HD;
  }
  return dot(vA, E_ * vB);
}

/* ************************************************************************* */

} // gtsam
