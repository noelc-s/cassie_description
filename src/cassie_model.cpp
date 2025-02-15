/*
 * MIT License
 * 
 * Copyright (c) 2020 Jenna Reher (jreher@caltech.edu)
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
*/

#include <cassie_description/cassie_model.hpp>
#include "pinocchio/parsers/urdf.hpp"
#include "pinocchio/algorithm/joint-configuration.hpp"
#include "pinocchio/algorithm/kinematics.hpp"
#include "pinocchio/algorithm/aba.hpp"
#include "pinocchio/algorithm/aba-derivatives.hpp"
#include "pinocchio/algorithm/contact-dynamics.hpp"
#include "pinocchio/container/aligned-vector.hpp"

#include <ros/ros.h>
#include <ros/package.h>
#include <rbdl/addons/urdfreader/urdfreader.h>
#include <Eigen/Geometry>
#include <fstream>

using namespace RigidBodyDynamics;
using namespace Eigen;

namespace cassie_model {

void BodyPoint::initialize(Model *model, std::string point_name, std::string body_name, Eigen::Vector3d &point_position) {
    this->body_name = body_name;
    this->point_name = point_name;
    this->point_position << point_position;
    this->body_id = model->GetBodyId(body_name.c_str());
} // BodyPoint::initialize

Kinematics::Kinematics() {
    this->cache.init();
}

void Kinematics::Cache::init() {
    this->J_positionLeft.resize(3,22);
    this->J_positionRight.resize(3,22);
    this->J_leftLegLength.resize(1,7);
    this->J_rightLegLength.resize(1,7);
    this->J_poseLeft.resize(6,22);
    this->J_poseRight.resize(6,22);
    this->Jdot_poseLeft.resize(6,22);
    this->Jdot_poseRight.resize(6,22);

    this->J_poseLeftConstraint.resize(5,22);
    this->J_poseRightConstraint.resize(5,22);
    this->Jdot_poseLeftConstraint.resize(5,22);
    this->Jdot_poseRightConstraint.resize(5,22);
    this->J_achilles.resize(2,22);
    this->J_rigid.resize(4,22);
    this->Jdot_achilles.resize(2,22);
    this->Jdot_rigid.resize(4,22);
}

void Kinematics::initialize(Model *model) {
    Vector3d left_midfoot_body_frame_position = Vector3d(0,0,0);
    this->LeftMidFoot_Point.initialize(model, "LeftMidFoot", "leftfoot_link", left_midfoot_body_frame_position);

    Vector3d right_midfoot_body_frame_position = Vector3d(0,0,0);
    this->RightMidFoot_Point.initialize(model, "RightMidFoot", "rightfoot_link", right_midfoot_body_frame_position);

    Vector3d left_heelspringend_body_frame_position = Vector3d(0,0,0);
    this->LeftHeel_Point.initialize(model, "LeftSpringEnd", "leftheelspring_link", left_heelspringend_body_frame_position);

    Vector3d rightHeelspringend_body_frame_position = Vector3d(0,0,0);
    this->RightHeel_Point.initialize(model, "RightSpringEnd", "rightheelspring_link", rightHeelspringend_body_frame_position);
} // Kinematics::initialize

void Kinematics::update(Model *model, const VectorXd &q, const VectorXd &dq) {
    //RigidBodyDynamics::UpdateKinematicsCustom(*model, &q, &dq, nullptr);
    this->cache.J_achilles.setZero();
    SymFunction::J_achilles_constraint(this->cache.J_achilles, q);
    this->cache.J_poseLeft.setZero();
    SymFunction::J_leftSole_constraint(this->cache.J_poseLeftConstraint, q);
    this->cache.J_poseRight.setZero();
    SymFunction::J_rightSole_constraint(this->cache.J_poseRightConstraint, q);
    this->cache.Jdot_achilles.setZero();
    SymFunction::Jdot_achilles_constraint(this->cache.Jdot_achilles, q, dq);
    this->cache.Jdot_poseLeft.setZero();
    SymFunction::Jdot_leftSole_constraint(this->cache.Jdot_poseLeftConstraint, q, dq);
    this->cache.Jdot_poseRight.setZero();
    SymFunction::Jdot_rightSole_constraint(this->cache.Jdot_poseRightConstraint, q, dq);

    MatrixXd temp_left(2,22), temp_right(2,22);
    temp_left.setZero(); temp_right.setZero();
    SymFunction::J_left_fixed_constraint(temp_left, q);
    SymFunction::J_right_fixed_constraint(temp_right, q);
    this->cache.J_rigid << temp_left, temp_right;

    temp_left.setZero(); temp_right.setZero();
    SymFunction::Jdot_left_fixed_constraint(temp_left, q, dq);
    SymFunction::Jdot_right_fixed_constraint(temp_right, q, dq);
    this->cache.Jdot_rigid << temp_left, temp_right;
} // Kinematics::update

void Kinematics::computeConstrainedToeJacobian(Eigen::VectorXd &q, Eigen::MatrixXd &Jl, Eigen::MatrixXd &Jr) {
    assert_size_vector(q, 22);
    assert_size_matrix(Jl, 3,7);
    assert_size_matrix(Jr, 3,7);

    Kinematics::Cache *ca = &this->cache;

    SymFunction::J_leftToe(ca->J_positionLeft, q);
    SymFunction::J_rightToe(ca->J_positionRight, q);
    SymFunction::J_achilles_constraint(ca->J_achilles, q);

    MatrixXd Jlf_active(3,7), Jrf_active(3,7), Jach_active_lf(1,7), Jach_active_rf(1,7);

    Jlf_active << ca->J_positionLeft.block(0, CassieStateEnum::LeftHipRoll, 3, 1),
                  ca->J_positionLeft.block(0, CassieStateEnum::LeftHipYaw, 3, 1),
                  ca->J_positionLeft.block(0, CassieStateEnum::LeftHipPitch, 3, 1),
                  ca->J_positionLeft.block(0, CassieStateEnum::LeftKneePitch, 3, 1),
                  ca->J_positionLeft.block(0, CassieStateEnum::LeftShinPitch, 3, 1),
                  ca->J_positionLeft.block(0, CassieStateEnum::LeftHeelSpring, 3, 1),
                  ca->J_positionLeft.block(0, CassieStateEnum::LeftFootPitch, 3, 1);
    Jrf_active << ca->J_positionRight.block(0, CassieStateEnum::RightHipRoll, 3, 1),
                  ca->J_positionRight.block(0, CassieStateEnum::RightHipYaw, 3, 1),
                  ca->J_positionRight.block(0, CassieStateEnum::RightHipPitch, 3, 1),
                  ca->J_positionRight.block(0, CassieStateEnum::RightKneePitch, 3, 1),
                  ca->J_positionRight.block(0, CassieStateEnum::RightShinPitch, 3, 1),
                  ca->J_positionRight.block(0, CassieStateEnum::RightHeelSpring, 3, 1),
                  ca->J_positionRight.block(0, CassieStateEnum::RightFootPitch, 3, 1);

    Jach_active_lf << ca->J_achilles.block(0, CassieStateEnum::LeftHipRoll, 1, 1),
                      ca->J_achilles.block(0, CassieStateEnum::LeftHipYaw, 1, 1),
                      ca->J_achilles.block(0, CassieStateEnum::LeftHipPitch, 1, 1),
                      ca->J_achilles.block(0, CassieStateEnum::LeftKneePitch, 1, 1),
                      ca->J_achilles.block(0, CassieStateEnum::LeftShinPitch, 1, 1),
                      ca->J_achilles.block(0, CassieStateEnum::LeftHeelSpring, 1, 1),
                      ca->J_achilles.block(0, CassieStateEnum::LeftFootPitch, 1, 1);
    Jach_active_rf << ca->J_achilles.block(1, CassieStateEnum::RightHipRoll, 1, 1),
                      ca->J_achilles.block(1, CassieStateEnum::RightHipYaw, 1, 1),
                      ca->J_achilles.block(1, CassieStateEnum::RightHipPitch, 1, 1),
                      ca->J_achilles.block(1, CassieStateEnum::RightKneePitch, 1, 1),
                      ca->J_achilles.block(1, CassieStateEnum::RightShinPitch, 1, 1),
                      ca->J_achilles.block(1, CassieStateEnum::RightHeelSpring, 1, 1),
                      ca->J_achilles.block(1, CassieStateEnum::RightFootPitch, 1, 1);

    Jl = Jlf_active - ca->J_positionLeft.block(0, CassieStateEnum::LeftTarsusPitch, 3, 1) / ca->J_achilles(0, CassieStateEnum::LeftTarsusPitch) * Jach_active_lf;
    Jr = Jrf_active - ca->J_positionRight.block(0, CassieStateEnum::RightTarsusPitch, 3, 1) / ca->J_achilles(1, CassieStateEnum::RightTarsusPitch) * Jach_active_rf;
} // Kinematics::computeConstrainedToeJacobian

void Kinematics::computeConstrainedFootJacobian(Eigen::VectorXd &q, Eigen::MatrixXd &Jl, Eigen::MatrixXd &Jr) {
    assert_size_vector(q, 22);
    assert_size_matrix(Jl, 3,7);
    assert_size_matrix(Jr, 3,7);

    Kinematics::Cache *ca = &this->cache;

    SymFunction::J_leftFoot(ca->J_poseLeft, q);
    SymFunction::J_rightFoot(ca->J_poseRight, q);
    SymFunction::J_achilles_constraint(ca->J_achilles, q);

    MatrixXd Jlf_active(3,7), Jrf_active(3,7), Jach_active_lf(1,7), Jach_active_rf(1,7);

    Jlf_active << ca->J_poseLeft.block(0, CassieStateEnum::LeftHipRoll, 3, 1),
                  ca->J_poseLeft.block(0, CassieStateEnum::LeftHipYaw, 3, 1),
                  ca->J_poseLeft.block(0, CassieStateEnum::LeftHipPitch, 3, 1),
                  ca->J_poseLeft.block(0, CassieStateEnum::LeftKneePitch, 3, 1),
                  ca->J_poseLeft.block(0, CassieStateEnum::LeftShinPitch, 3, 1),
                  ca->J_poseLeft.block(0, CassieStateEnum::LeftHeelSpring, 3, 1),
                  ca->J_poseLeft.block(0, CassieStateEnum::LeftFootPitch, 3, 1);
    Jrf_active << ca->J_poseRight.block(0, CassieStateEnum::RightHipRoll, 3, 1),
                  ca->J_poseRight.block(0, CassieStateEnum::RightHipYaw, 3, 1),
                  ca->J_poseRight.block(0, CassieStateEnum::RightHipPitch, 3, 1),
                  ca->J_poseRight.block(0, CassieStateEnum::RightKneePitch, 3, 1),
                  ca->J_poseRight.block(0, CassieStateEnum::RightShinPitch, 3, 1),
                  ca->J_poseRight.block(0, CassieStateEnum::RightHeelSpring, 3, 1),
                  ca->J_poseRight.block(0, CassieStateEnum::RightFootPitch, 3, 1);

    Jach_active_lf << ca->J_achilles.block(0, CassieStateEnum::LeftHipRoll, 1, 1),
                      ca->J_achilles.block(0, CassieStateEnum::LeftHipYaw, 1, 1),
                      ca->J_achilles.block(0, CassieStateEnum::LeftHipPitch, 1, 1),
                      ca->J_achilles.block(0, CassieStateEnum::LeftKneePitch, 1, 1),
                      ca->J_achilles.block(0, CassieStateEnum::LeftShinPitch, 1, 1),
                      ca->J_achilles.block(0, CassieStateEnum::LeftHeelSpring, 1, 1),
                      ca->J_achilles.block(0, CassieStateEnum::LeftFootPitch, 1, 1);
    Jach_active_rf << ca->J_achilles.block(1, CassieStateEnum::RightHipRoll, 1, 1),
                      ca->J_achilles.block(1, CassieStateEnum::RightHipYaw, 1, 1),
                      ca->J_achilles.block(1, CassieStateEnum::RightHipPitch, 1, 1),
                      ca->J_achilles.block(1, CassieStateEnum::RightKneePitch, 1, 1),
                      ca->J_achilles.block(1, CassieStateEnum::RightShinPitch, 1, 1),
                      ca->J_achilles.block(1, CassieStateEnum::RightHeelSpring, 1, 1),
                      ca->J_achilles.block(1, CassieStateEnum::RightFootPitch, 1, 1);

    Jl = Jlf_active - ca->J_poseLeft.block(0, CassieStateEnum::LeftTarsusPitch, 3, 1) / ca->J_achilles(0, CassieStateEnum::LeftTarsusPitch) * Jach_active_lf;
    Jr = Jrf_active - ca->J_poseRight.block(0, CassieStateEnum::RightTarsusPitch, 3, 1) / ca->J_achilles(1, CassieStateEnum::RightTarsusPitch) * Jach_active_rf;
} // Kinematics::computeConstrainedFootJacobian

void Kinematics::computeStanceConstrainedJacobian(Eigen::VectorXd &q, Eigen::MatrixXd &Jl, Eigen::MatrixXd &Jr) {
    assert_size_vector(q, 22);
    assert_size_matrix(Jl, 5,7);
    assert_size_matrix(Jr, 5,7);

    Kinematics::Cache *ca = &this->cache;

    // This assumes that kinematics.update() has been called!

    MatrixXd Jlf_active(5,7), Jrf_active(5,7), Jach_active_lf(1,7), Jach_active_rf(1,7);

    Jlf_active << ca->J_poseLeftConstraint.block(0, CassieStateEnum::LeftHipRoll, 5, 1),
                  ca->J_poseLeftConstraint.block(0, CassieStateEnum::LeftHipYaw, 5, 1),
                  ca->J_poseLeftConstraint.block(0, CassieStateEnum::LeftHipPitch, 5, 1),
                  ca->J_poseLeftConstraint.block(0, CassieStateEnum::LeftKneePitch, 5, 1),
                  ca->J_poseLeftConstraint.block(0, CassieStateEnum::LeftShinPitch, 5, 1),
                  ca->J_poseLeftConstraint.block(0, CassieStateEnum::LeftHeelSpring, 5, 1),
                  ca->J_poseLeftConstraint.block(0, CassieStateEnum::LeftFootPitch, 5, 1);
    Jrf_active << ca->J_poseRightConstraint.block(0, CassieStateEnum::RightHipRoll, 5, 1),
                  ca->J_poseRightConstraint.block(0, CassieStateEnum::RightHipYaw, 5, 1),
                  ca->J_poseRightConstraint.block(0, CassieStateEnum::RightHipPitch, 5, 1),
                  ca->J_poseRightConstraint.block(0, CassieStateEnum::RightKneePitch, 5, 1),
                  ca->J_poseRightConstraint.block(0, CassieStateEnum::RightShinPitch, 5, 1),
                  ca->J_poseRightConstraint.block(0, CassieStateEnum::RightHeelSpring, 5, 1),
                  ca->J_poseRightConstraint.block(0, CassieStateEnum::RightFootPitch, 5, 1);

    Jach_active_lf << ca->J_achilles.block(0, CassieStateEnum::LeftHipRoll, 1, 1),
                      ca->J_achilles.block(0, CassieStateEnum::LeftHipYaw, 1, 1),
                      ca->J_achilles.block(0, CassieStateEnum::LeftHipPitch, 1, 1),
                      ca->J_achilles.block(0, CassieStateEnum::LeftKneePitch, 1, 1),
                      ca->J_achilles.block(0, CassieStateEnum::LeftShinPitch, 1, 1),
                      ca->J_achilles.block(0, CassieStateEnum::LeftHeelSpring, 1, 1),
                      ca->J_achilles.block(0, CassieStateEnum::LeftFootPitch, 1, 1);
    Jach_active_rf << ca->J_achilles.block(1, CassieStateEnum::RightHipRoll, 1, 1),
                      ca->J_achilles.block(1, CassieStateEnum::RightHipYaw, 1, 1),
                      ca->J_achilles.block(1, CassieStateEnum::RightHipPitch, 1, 1),
                      ca->J_achilles.block(1, CassieStateEnum::RightKneePitch, 1, 1),
                      ca->J_achilles.block(1, CassieStateEnum::RightShinPitch, 1, 1),
                      ca->J_achilles.block(1, CassieStateEnum::RightHeelSpring, 1, 1),
                      ca->J_achilles.block(1, CassieStateEnum::RightFootPitch, 1, 1);

    Jl = Jlf_active - ca->J_poseLeft.block(0, CassieStateEnum::LeftTarsusPitch, 5, 1) / ca->J_achilles(0, CassieStateEnum::LeftTarsusPitch) * Jach_active_lf;
    Jr = Jrf_active - ca->J_poseRight.block(0, CassieStateEnum::RightTarsusPitch, 5, 1) / ca->J_achilles(1, CassieStateEnum::RightTarsusPitch) * Jach_active_rf;
} // Kinematics::computeConstrainedFootJacobian

void Dynamics::initialize(Model *model) {
    this->H.resize(model->dof_count,model->dof_count);
    this->C.resize(model->dof_count);
    this->H.setZero();
    this->C.setZero();
} // Dynamics::initialize

void Dynamics::calcHandC(Model *model, VectorXd &Q, VectorXd &QDot) {
    // Evaluate inertia matrix (H) and velocity terms (C)
    VectorXd QDDot(22);
    QDDot.setZero();
    // InverseDynamics(*model, Q, QDot, QDDot, this->C);
    NonlinearEffects(*model,Q,QDot,this->C); // compute Coriolis
    CompositeRigidBodyAlgorithm(*model, Q, this->H, false);

    // Add reflected rotor inertias
    this->H(LeftHipRoll,LeftHipRoll)       += 0.038125;
    this->H(RightHipRoll,RightHipRoll)     += 0.038125;
    this->H(LeftHipYaw,LeftHipYaw)         += 0.038125;
    this->H(RightHipYaw,RightHipYaw)       += 0.038125;
    this->H(LeftHipPitch,LeftHipPitch)     += 0.09344;
    this->H(RightHipPitch,RightHipPitch)   += 0.09344;
    this->H(LeftKneePitch,LeftKneePitch)   += 0.09344;
    this->H(RightKneePitch,RightKneePitch) += 0.09344;
    this->H(LeftFootPitch,LeftFootPitch)   += 0.01225;
    this->H(RightFootPitch,RightFootPitch) += 0.01225;
} // Dynamics::calcHandC

void Linearizations::initialize(pinocchio::Model *pmodel) {
    this->A.resize(44,44);
    this->B.resize(44,10);
    this->C.resize(44);
}

void Linearizations::calcLinearizations(pinocchio::Model *pmodel, VectorXd &Q_bar,VectorXd &QDot_bar, VectorXd &U_bar, MatrixXd Be, MatrixXd Jc, MatrixXd dJc) {
    // NonlinearEffects(*model,Q,QDot,this->C); // compute Coriolis
    // CompositeRigidBodyAlgorithm(*model, Q, this->H, false);    

    // this->H_inv = H.inverse();

    // SymFunction::Df(this->Df, Q_bar, QDot_bar);
    // this->f = this->H_inv*this->C;

    Quaterniond q;
    q = AngleAxisd(Q_bar(3), Vector3d::UnitX())
        * AngleAxisd(Q_bar(4), Vector3d::UnitY())
        * AngleAxisd(Q_bar(5), Vector3d::UnitZ());

    VectorXd Q_bar_quat(23);
    Q_bar_quat << Q_bar.segment(0,3), q.coeffs(), Q_bar.segment(6,16);

    pinocchio::Data data_nonlinear(*pmodel);
    pinocchio::Data data_jacobian(*pmodel);

    // Allocate result container
    Eigen::MatrixXd djoint_acc_dq = Eigen::MatrixXd::Zero(22,22);
    Eigen::MatrixXd djoint_acc_dv = Eigen::MatrixXd::Zero(22,22);
    Eigen::MatrixXd djoint_acc_dtau = Eigen::MatrixXd::Zero(22,22);

    pinocchio::forwardDynamics(*pmodel, data_nonlinear, Q_bar_quat, QDot_bar, Be*U_bar, Jc, dJc*QDot_bar, 1e-12);

    VectorXd lambda = data_nonlinear.lambda_c;
    VectorXd zero = VectorXd::Zero(6);
    VectorXd left(6);
    VectorXd right(6);
    left << lambda(6),lambda(7),lambda(8),0,lambda(9),lambda(10);
    right << lambda(11),lambda(12),lambda(13),0,lambda(14),lambda(15);

    pinocchio::Force zeroForce(zero);
    pinocchio::Force leftForce(left);
    pinocchio::Force rightForce(right);

    pinocchio::container::aligned_vector<pinocchio::Force> forces;
    for (int i = 0; i < 9; i ++)
        forces.push_back(zeroForce);
    forces.push_back(leftForce);
    for (int i = 10; i < 17; i ++)
        forces.push_back(zeroForce);
    forces.push_back(rightForce);

    // TODO: what is J_rigid??
    computeABADerivatives(*pmodel, data_jacobian, Q_bar_quat, QDot_bar, Be*U_bar, forces, djoint_acc_dq, djoint_acc_dv, djoint_acc_dtau);
    this->A << MatrixXd::Zero(22,22), MatrixXd::Identity(22,22), djoint_acc_dq, djoint_acc_dv;
    this->B << MatrixXd::Zero(22,10), djoint_acc_dtau*Be;
//    aba(*pmodel, data, Q_bar_quat, QDot_bar, Be*U_bar);
    this->C << Eigen::VectorXd::Zero(22), data_nonlinear.ddq - (djoint_acc_dq*Q_bar + djoint_acc_dv*QDot_bar) - djoint_acc_dtau*Be*U_bar;


    //    pinocchio::aba(*(this->robot->pmodel), data, Q_bar_quat, QDot, this->config.Be*this->cache.u);

    //    pinocchio::computeJointJacobians(*(this->robot->pmodel), data, Q_bar_quat);

    //    MatrixXd Jac = data.J.transpose();
    //    VectorXd F = Jac.householderQr().solve(this->config.Be*this->cache.u);

    // Compute the robot constraints

//    this->cache.u << torqueScale.cwiseProduct(this->cache.u);
}

Cassie::Cassie(bool verbose) {
    // Retreive the path to the robot URDF description
    std::string urdf_path = ros::package::getPath("cassie_description");
    urdf_path.append("/urdf/cassie_v4.urdf");

    const char* urdf_path_cstr = urdf_path.c_str();
    const std::string urdf_path_c = "/home/jreher/cassie_ws/src/cassie_description/urdf/cassie_v4.urdf";

    // Construct the RBDL model
    this->model = new RigidBodyDynamics::Model();
    Addons::URDFReadFromFile(urdf_path_cstr, this->model, true, verbose);

    // Construct Pinocchio model
    this->pmodel = new pinocchio::Model();
    pinocchio::urdf::buildModel(urdf_path_c,pinocchio::JointModelFreeFlyer(), *(this->pmodel));

    // Add the leg loop constraints
    // TODO: This is not fully integrated.
    this->constraints.AddLoopConstraint(
        this->model->GetBodyId("lefthippitch"),
        this->model->GetBodyId("leftheelspring"),
        Math::SpatialTransform(Matrix3d::Identity(3,3), Vector3d(0., 0., 0.045)),
        Math::SpatialTransform(Matrix3d::Identity(3,3), Vector3d(0.11877, -0.01, 0.)),
        Math::SpatialVector(0.,0.,0.,1.,1.,1.),
        false,
        0.1,
        "leftachilles");
    this->constraints.AddLoopConstraint(
        this->model->GetBodyId("righthippitch"),
        this->model->GetBodyId("rightheelspring"),
        Math::SpatialTransform(Matrix3d::Identity(3,3), Vector3d(0., 0., -0.045)),
        Math::SpatialTransform(Matrix3d::Identity(3,3), Vector3d(0.11877, -0.01, 0.)),
        Math::SpatialVector(0.,0.,0.,1.,1.,1.),
        false,
        0.1,
        "rightachilles");
    this->constraints.Bind(*this->model);
    this->constraints.clear();

    this->torque.resize(10);
    this->torque.setZero();

    // Create robot statespace
    this->q.resize(22);
    this->dq.resize(22);
    this->ddq.resize(22);

    this->q.setZero();
    this->dq.setZero();
    this->ddq.setZero();
    this->quat_pelvis.setIdentity();

    // Indexing helpers
    this->iRotorMap.resize(10);
    this->iJointMap.resize(4);
    this->iEncoderMap.resize(14);
    this->iRotorMap <<
            CassieStateEnum::LeftHipRoll,
            CassieStateEnum::LeftHipYaw,
            CassieStateEnum::LeftHipPitch,
            CassieStateEnum::LeftKneePitch,
            CassieStateEnum::LeftFootPitch,
            CassieStateEnum::RightHipRoll,
            CassieStateEnum::RightHipYaw,
            CassieStateEnum::RightHipPitch,
            CassieStateEnum::RightKneePitch,
            CassieStateEnum::RightFootPitch;
    this->iJointMap <<
            CassieStateEnum::LeftShinPitch,
            CassieStateEnum::LeftTarsusPitch,
            CassieStateEnum::RightShinPitch,
            CassieStateEnum::RightTarsusPitch;
    this->iEncoderMap <<
            CassieStateEnum::LeftHipRoll,
            CassieStateEnum::LeftHipYaw,
            CassieStateEnum::LeftHipPitch,
            CassieStateEnum::LeftKneePitch,
            CassieStateEnum::LeftShinPitch,
            CassieStateEnum::LeftTarsusPitch,
            CassieStateEnum::LeftFootPitch,
            CassieStateEnum::RightHipRoll,
            CassieStateEnum::RightHipYaw,
            CassieStateEnum::RightHipPitch,
            CassieStateEnum::RightKneePitch,
            CassieStateEnum::RightShinPitch,
            CassieStateEnum::RightTarsusPitch,
            CassieStateEnum::RightFootPitch;

    // Append additional modeling considerations
    this->model->gravity = Vector3d(0., 0., -9.80665);

    // Contact modes
    this->leftContact  = 0.0;
    this->rightContact = 0.0;

    this->GRF.resize(6);
    this->GRF = Eigen::VectorXd::Zero(6);

    // Construct the class members
    this->kinematics.initialize(this->model);
    this->dynamics.initialize(this->model);
    this->linearizations.initialize(this->pmodel);
} // Cassie::Cassie

} // namespace cassie_model
