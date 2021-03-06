// =============================================================================
// PROJECT CHRONO - http://projectchrono.org
//
// Copyright (c) 2014 projectchrono.org
// All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file at the top level of the distribution and at
// http://projectchrono.org/license-chrono.txt.
//
// =============================================================================
// Authors: Radu Serban
// =============================================================================
//
// Demonstration of a steering path-follower PID controller.
//
// The vehicle reference frame has Z up, X towards the front of the vehicle, and
// Y pointing to the left.
//
// =============================================================================

// C/C++ library
#include <iostream>
#include <math.h>
#include <vector>
#include <string>
#include <unistd.h>

// Computing tool library
#include "interpolation.h"
#include "PID.h"

// ROS include library
#include "ros/ros.h"
#include "std_msgs/Float64.h"
#include "nloptcontrol_planner/Trajectory.h"
#include "ros_chrono_msgs/veh_status.h"

// Chrono include library
#include "chrono/core/ChFileutils.h"
#include "chrono/core/ChRealtimeStep.h"
#include "chrono/utils/ChFilters.h"

#include "chrono_vehicle/ChVehicleModelData.h"
#include "chrono_vehicle/terrain/RigidTerrain.h"
#include "chrono_vehicle/driver/ChIrrGuiDriver.h"
#include "chrono_vehicle/driver/ChPathFollowerDriver.h"
#include "chrono_vehicle/utils/ChVehiclePath.h"
#include "chrono_vehicle/wheeled_vehicle/utils/ChWheeledVehicleIrrApp.h"
#include "chrono_models/vehicle/hmmwv/HMMWV.h"


#include "chrono_thirdparty/rapidjson/document.h"
#include "chrono_thirdparty/rapidjson/filereadstream.h"

using namespace chrono;
using namespace chrono::geometry;
using namespace chrono::vehicle;
using namespace chrono::vehicle::hmmwv;
using namespace alglib;
using namespace rapidjson;


// to run demo_steering.launch
//change at 10/31/2018
std::string data_path("../../../src/models/chrono/ros_chrono/src/data/vehicle/");

// Rigid terrain dimensions
double terrainHeight = 0;
double terrainLength = 500.0;  // size in X direction
double terrainWidth = 500.0;   // size in Y direction


// ROS Control Input (Topic)
std::vector<double> traj_t(2,0);
std::vector<double> traj_x(2,0);
std::vector<double> traj_y(2,0);
std::vector<double> traj_psi(2,0);
std::vector<double> traj_sa(2,0);
std::vector<double> traj_ux(2,0);

// Interpolator
real_1d_array t_array;
real_1d_array sa_array;
real_1d_array ux_array;
spline1dinterpolant s_ux;
spline1dinterpolant s_sa;
double time_counter = 0.0;
double traj_sa_interp = 0.0;
double traj_ux_interp = 0.0;
int current_index = 0;

// ------
// Chrono
// ------
// Contact method type
ChMaterialSurface::ContactMethod contact_method = ChMaterialSurface::SMC;

// Type of tire model (RIGID, LUGRE, FIALA, PACEJKA, or TMEASY)
TireModelType tire_model = TireModelType::RIGID;

// Type of powertrain model (SHAFTS or SIMPLE)
PowertrainModelType powertrain_model = PowertrainModelType::SHAFTS;

// Drive type (FWD, RWD, or AWD)
DrivelineType drive_type = DrivelineType::RWD;

// Steering type (PITMAN_ARM or PITMAN_ARM_SHAFTS)
// Note: Compliant steering requires higher PID gains.
SteeringType steering_type = SteeringType::PITMAN_ARM;

// Visualization type for vehicle parts (PRIMITIVES, MESH, or NONE)
VisualizationType chassis_vis_type = VisualizationType::PRIMITIVES;
VisualizationType suspension_vis_type = VisualizationType::PRIMITIVES;
VisualizationType steering_vis_type = VisualizationType::PRIMITIVES;
VisualizationType wheel_vis_type = VisualizationType::PRIMITIVES;
VisualizationType tire_vis_type = VisualizationType::NONE;

// Point on chassis tracked by the chase camera
ChVector<> trackPoint(0.0, 0.0, 1.75);

// =============================================================================

void plannerCallback(const nloptcontrol_planner::Trajectory::ConstPtr& control_msgs) {
    ROS_INFO("Trajectory messages received");
    traj_t = control_msgs->t;
    traj_x = control_msgs->x;
    traj_y = control_msgs->y;
    traj_psi = control_msgs->psi;
    traj_sa = control_msgs->sa;
    traj_ux = control_msgs->ux;

    // Reset time_counter
    time_counter = 0;
    current_index = 0;
    //ROS_INFO("New callback");
    /*
    for (int i = 0; i < traj_t.size(); i++) {
        std::cout << traj_t[i] - traj_t[0] <<" "<<traj_ux[i]<<std::endl;
        //ROS_INFO("ref: t = %0.4f, u = %0.4f", traj_t[i] - traj_t[0], traj_ux[i]);
    }
    ROS_INFO("End new callback");*/
}


// =============================================================================

int main(int argc, char* argv[]) {

    // ------------------------------
    // Initialize ROS node handle
    // ------------------------------
    ros::init(argc, argv, "steering_controller");
    ros::NodeHandle node;

    // Declare ROS subscriber to subscribe planner topic
    std::string planner_namespace;
    node.getParam("system/planner",planner_namespace);
    ros::Subscriber planner_sub = node.subscribe(planner_namespace + "/control", 100, plannerCallback);

    // Declare ROS publisher to advertise vehicleinfo topic
    std::string chrono_namespace;
    node.getParam("system/chrono/namespace", chrono_namespace);
    ros::Publisher vehicleinfo_pub = node.advertise<ros_chrono_msgs::veh_status>(chrono_namespace+"/vehicleinfo", 1);
    ros_chrono_msgs::veh_status vehicleinfo_data;


    // Define variables for ROS parameters server
    double step_size = 5e-3;
    double tire_step_size = step_size / 2.0;
    double goal_tol = 0.1; // Path following tolerance
    double target_speed = 0.0; // Desired velocity
    double x0 = 0.0, y0 = 0.0, z0 = 0.5; // Initial global position
    double roll0 = 0.0, pitch0 = 0.0, yaw0 = 0.0; // Initial global orientation
    double x = 0, y = 0;
    double goal_x = 0, goal_y = 0; // Goal position

    // Get parameters from ROS Parameter Server
    node.getParam("system/params/step_size", step_size); // ROS loop rate and Chrono step size
    node.getParam("system/params/goal_tol",goal_tol);

    node.getParam("case/actual/X0/x", x0); // initial x
    node.getParam("case/actual/X0/yVal", y0); // initial y
    node.getParam("case/actual/X0/psi", yaw0); // initial yaw angle
  //  node.getParam("case/actual/X0/v", v); // lateral velocity

    node.getParam("case/actual/X0/theta", pitch0); // initial pitch
    node.getParam("case/actual/X0/phi", roll0); // initial roll
    node.getParam("case/actual/X0/z", z0); // initial z


    // Load chrono vehicle_params
    double frict_coeff, rest_coeff, gear_ratios;
    std::vector<double> centroidLoc, centroidOrientation;
    double chassisMass;
    std::vector<double> chassisInertia;
    std::vector<double> driverLoc, driverOrientation;
    std::vector<double> motorBlockDirection, axleDirection;
    double driveshaftInertia, differentialBoxInertia, conicalGearRatio, differentialRatio;
    std::vector<double> gearRatios;
    double steeringLinkMass, steeringLinkRadius, steeringLinkLength;
    std::vector<double> steeringLinkInertia;
    double pinionRadius, pinionMaxAngle, maxBrakeTorque;
    node.getParam("vehicle/chrono/vehicle_params/frict_coeff", frict_coeff);
    node.getParam("vehicle/chrono/vehicle_params/rest_coeff", rest_coeff);
    node.getParam("vehicle/chrono/vehicle_params/gearRatios", gear_ratios);
    node.getParam("vehicle/chrono/vehicle_params/centroidLoc", centroidLoc);
    node.getParam("vehicle/chrono/vehicle_params/centroidOrientation", centroidOrientation);
    node.getParam("vehicle/chrono/vehicle_params/chassisMass", chassisMass);
    node.getParam("vehicle/chrono/vehicle_params/chassisInertia", chassisInertia);
    node.getParam("vehicle/chrono/vehicle_params/driverLoc", driverLoc);
    node.getParam("vehicle/chrono/vehicle_params/driverOrientation", driverOrientation);
    node.getParam("vehicle/chrono/vehicle_params/motorBlockDirection", motorBlockDirection);
    node.getParam("vehicle/chrono/vehicle_params/axleDirection", axleDirection);
    node.getParam("vehicle/chrono/vehicle_params/driveshaftInertia", driveshaftInertia);
    node.getParam("vehicle/chrono/vehicle_params/differentialBoxInertia", differentialBoxInertia);
    node.getParam("vehicle/chrono/vehicle_params/conicalGearRatio", conicalGearRatio);
    node.getParam("vehicle/chrono/vehicle_params/differentialRatio", differentialRatio);
    node.getParam("vehicle/chrono/vehicle_params/gearRatios", gearRatios);
    node.getParam("vehicle/chrono/vehicle_params/steeringLinkMass", steeringLinkMass);
    node.getParam("vehicle/chrono/vehicle_params/steeringLinkInertia", steeringLinkInertia);
    node.getParam("vehicle/chrono/vehicle_params/steeringLinkRadius", steeringLinkRadius);
    node.getParam("vehicle/chrono/vehicle_params/steeringLinkLength", steeringLinkLength);
    node.getParam("vehicle/chrono/vehicle_params/pinionRadius", pinionRadius);
    node.getParam("vehicle/chrono/vehicle_params/pinionMaxAngle", pinionMaxAngle);
    node.getParam("vehicle/chrono/vehicle_params/maxBrakeTorque", maxBrakeTorque);

    // ---------------------
    // Set up PID controller
    // ---------------------
    // PID controller
    double Kp = 0.5, Ki = 0.1, Kd = 0.0, Kw = 0.0, time_shift = 3.0; // PID controller parameter
    std::string windup_method("clamping"); // Anti-windup method
    double controller_output = 0.0;
    double controller_output2 = 0.0;
    PID controller;

    node.getParam("controller/Kp",Kp);
    node.getParam("controller/Ki",Ki);
    node.getParam("controller/Kd",Kd);
    node.getParam("controller/Kw",Kw);
    node.getParam("controller/anti_windup", windup_method);
    node.getParam("controller/time_shift", time_shift);

    controller.set_PID(Kp, Ki, Kd, Kw);
    controller.set_step_size(step_size);
    controller.set_output_limit(-1.0, 1.0);
    controller.set_windup_metohd(windup_method);
    controller.initialize();


    // Declare loop rate
    ros::Rate loop_rate(int(1.0/step_size));

    // Set initial vehicle location and orientation
    ChVector<> initLoc(x0, y0, z0);
    // ChQuaternion<> initRot(q[0],q[1],q[2],q[3]);

    double t0 = std::cos(yaw0 * 0.5f);
    double t1 = std::sin(yaw0 * 0.5f);
    double t2 = std::cos(roll0 * 0.5f);
    double t3 = std::sin(roll0 * 0.5f);
    double t4 = std::cos(pitch0 * 0.5f);
    double t5 = std::sin(pitch0 * 0.5f);

    double q0_0 = t0 * t2 * t4 + t1 * t3 * t5;
    double q0_1 = t0 * t3 * t4 - t1 * t2 * t5;
    double q0_2 = t0 * t2 * t5 + t1 * t3 * t4;
    double q0_3 = t1 * t2 * t4 - t0 * t3 * t5;

    ChQuaternion<> initRot(q0_0,q0_1,q0_2,q0_3);

    // ------------------------------
    // Create the vehicle and terrain
    // ------------------------------

    // Create the HMMWV vehicle, set parameters, and initialize
    HMMWV_Full my_hmmwv;
    my_hmmwv.SetContactMethod(contact_method);
    my_hmmwv.SetChassisFixed(false);
    my_hmmwv.SetInitPosition(ChCoordsys<>(initLoc, initRot));
    my_hmmwv.SetPowertrainType(powertrain_model);
    my_hmmwv.SetDriveType(drive_type);
    my_hmmwv.SetSteeringType(steering_type);
    my_hmmwv.SetTireType(tire_model);
    my_hmmwv.SetTireStepSize(tire_step_size);
    my_hmmwv.SetVehicleStepSize(step_size);
    my_hmmwv.Initialize();

    my_hmmwv.SetChassisVisualizationType(chassis_vis_type);
    my_hmmwv.SetSuspensionVisualizationType(suspension_vis_type);
    my_hmmwv.SetSteeringVisualizationType(steering_vis_type);
    my_hmmwv.SetWheelVisualizationType(wheel_vis_type);
    my_hmmwv.SetTireVisualizationType(tire_vis_type);

    // Create the terrain
    RigidTerrain terrain(my_hmmwv.GetSystem());
    auto patch = terrain.AddPatch(ChCoordsys<>(ChVector<>(0, 0, terrainHeight - 5), QUNIT),
                                  ChVector<>(terrainLength, terrainWidth, 10));
    patch->SetContactFrictionCoefficient(frict_coeff);
    patch->SetContactRestitutionCoefficient(rest_coeff);
    patch->SetContactMaterialProperties(2e7f, 0.3f);
    patch->SetColor(ChColor(1, 1, 1));
    patch->SetTexture(data_path+"terrain/textures/tile4.jpg", 200, 200);
    terrain.Initialize();

    // ---------------------------------------
    // Create the vehicle Irrlicht application
    // ---------------------------------------

    ChVehicleIrrApp app(&my_hmmwv.GetVehicle(), &my_hmmwv.GetPowertrain(), L"Steering PID Controller Demo",
                        irr::core::dimension2d<irr::u32>(800, 640));

    app.SetHUDLocation(500, 20);
    app.AddTypicalLights(irr::core::vector3df(-150.f, -150.f, 200.f), irr::core::vector3df(-150.f, 150.f, 200.f), 100,
                         100);
    app.AddTypicalLights(irr::core::vector3df(150.f, -150.f, 200.f), irr::core::vector3df(150.0f, 150.f, 200.f), 100,
                         100);
    app.EnableGrid(false);
    app.SetChaseCamera(trackPoint, 6.0, 0.5);

    app.SetTimestep(step_size);

    // Finalize construction of visualization assets
    app.AssetBindAll();
    app.AssetUpdateAll();

    // ---------------
    // Simulation loop
    // ---------------

    // Driver location in vehicle local frame
    ChVector<> driver_pos = my_hmmwv.GetChassis()->GetLocalDriverCoordsys().pos;

    // Initialize simulation frame counter and simulation time
    ChRealtimeStepTimer realtime_timer;

    // Vehicle steering angle
    double speed = 0.0;
    double steering = 0.0;
    // Collect controller output data from modules (for inter-module communication)
    double throttle_input = 0;
    double steering_input = 0;
    double braking_input = 0;

    bool is_init;
    node.setParam("system/chrono/flags/initialized", true);
    node.getParam("system/flags/initialized", is_init);
    while (!is_init) {
        node.getParam("system/flags/initialized", is_init);
    }


    // read maximum_steering_angle
    double maximum_steering_angle;
    maximum_steering_angle = my_hmmwv.GetVehicle().GetMaxSteeringAngle();

    traj_t[1] = 20.0;

    while (ros::ok()) {
        // Extract chrono system state
        double time = my_hmmwv.GetSystem()->GetChTime();

        // --------------------------
        // interpolation using ALGLIB
        // --------------------------
        // Shift time to zero
        for (int i = 0; i < traj_t.size(); i++) {
            traj_t[i] = traj_t[i] - traj_t[0];
        }

        /*
        t_array.setcontent(traj_t.size(), &(traj_t[0]));
        sa_array.setcontent(traj_sa.size(), &(traj_sa[0]));
        ux_array.setcontent(traj_ux.size(), &(traj_ux[0]));
        spline1dbuildcubic(t_array, ux_array, s_ux);
        spline1dbuildcubic(t_array, sa_array, s_sa);
        traj_ux_interp = spline1dcalc(s_ux, time_counter);
        traj_sa_interp = spline1dcalc(s_sa, time_counter);*/

        while(current_index < (traj_t.size()-1) && time_counter > traj_t[current_index+1]){
            current_index++;
        }
        traj_ux_interp = (traj_ux[current_index+1]-traj_ux[current_index])/(traj_t[current_index+1]-traj_t[current_index])*(time_counter - traj_t[current_index]) + traj_ux[current_index];
        traj_sa_interp = (traj_sa[current_index+1]-traj_sa[current_index])/(traj_t[current_index+1]-traj_t[current_index])*(time_counter - traj_t[current_index]) + traj_sa[current_index];

        time_counter += step_size;
        //ROS_INFO("t = %0.4f, ux = %0.4f", time_counter, traj_ux_interp);
        //std::cout << time_counter << " "<<traj_ux_interp<<std::endl;

        // steering angle
        steering_input = traj_sa_interp / maximum_steering_angle;
        // steering_input = traj_sa[0] / maximum_steering_angle;
        if(steering_input < -1.0){
            steering_input = -1.0;
        }
        else if(steering_input > 1.0){
            steering_input = 1.0;
        }

        // PID controller output for throttle or brake
	    double ux_err = traj_ux_interp - speed;
        // double ux_err = traj_ux[0] - speed;
        controller_output = controller.control(ux_err);
        if (controller_output > 0){
            throttle_input = controller_output;
            braking_input = 0;
        }
        else {
            throttle_input = 0;
            braking_input = -controller_output;
        }

        app.BeginScene(true, true, irr::video::SColor(255, 140, 161, 192));
        app.DrawAll();

        // Update modules (process inputs from other modules)
        terrain.Synchronize(time);
        my_hmmwv.Synchronize(time, steering_input, braking_input, throttle_input, terrain);
        app.Synchronize("", steering_input, throttle_input, braking_input);

        // Advance simulation for one timestep for all modules
        double step = realtime_timer.SuggestSimulationStep(step_size);
        terrain.Advance(step);
        my_hmmwv.Advance(step);
        app.Advance(step);

        // Get vehicle information from Chrono vehicle model
        ChVector<> pos_global = my_hmmwv.GetVehicle().GetVehicleCOMPos();
        ChVector<> spd_global = my_hmmwv.GetChassisBody()->GetPos_dt();
        ChVector<> acc_global = my_hmmwv.GetChassisBody()->GetPos_dtdt();
        ChQuaternion<> rot_global = my_hmmwv.GetVehicle().GetVehicleRot();//global orientation as quaternion
        ChVector<> rot_dt = my_hmmwv.GetChassisBody()->GetWvel_loc(); //global orientation as quaternion

        steering = my_hmmwv.GetTire(0)->GetSlipAngle();
        speed = my_hmmwv.GetVehicle().GetVehicleSpeedCOM();

        double q0 = rot_global[0];
        double q1 = rot_global[1];
        double q2 = rot_global[2];
        double q3 = rot_global[3];

        double yaw_val=atan2(2*(q0*q3+q1*q2),1-2*(q2*q2+q3*q3));
        double theta_val=asin(2*(q0*q2-q3*q1));
        double phi_val= atan2(2*(q0*q1+q2*q3),1-2*(q1*q1+q2*q2));

        // Update vehicleinfo_data
        vehicleinfo_data.t_chrono = time; // time in chrono simulation
        vehicleinfo_data.x_pos = pos_global[0];
        vehicleinfo_data.y_pos = pos_global[1];
        vehicleinfo_data.x_v = spd_global[0];
        vehicleinfo_data.y_v = spd_global[1];
        vehicleinfo_data.x_a = acc_global[0];
        vehicleinfo_data.yaw_curr = yaw_val; // in radians
        vehicleinfo_data.yaw_rate = -rot_dt[2];// yaw rate
        vehicleinfo_data.sa = steering_input*maximum_steering_angle;
        vehicleinfo_data.thrt_in = throttle_input; // throttle input in the range [0,+1]
        vehicleinfo_data.brk_in = braking_input; // braking input in the range [0,+1]
        vehicleinfo_data.str_in = steering_input; // steeering input in the range [-1,+1]

        node.setParam("/state/t", time);
        node.setParam("/state/x", pos_global[0]);
        node.setParam("/state/y", pos_global[1]);
        node.setParam("/state/psi", yaw_val);
        node.setParam("/state/theta", theta_val);
        node.setParam("/state/phi", phi_val);
        node.setParam("/state/ux", speed);
        node.setParam("/state/v", spd_global[1]);
        node.setParam("/state/ax", acc_global[0]);
        node.setParam("/state/r", -rot_dt[2]);
        node.setParam("/state/sa", vehicleinfo_data.sa);
        node.setParam("/control/thr", throttle_input);
        node.setParam("/control/brk", braking_input);
        node.setParam("/control/str", steering_input);

        // Publish current vehicle information
        vehicleinfo_pub.publish(vehicleinfo_data);
        app.EndScene();

        ros::spinOnce();
        loop_rate.sleep();
    }

    return 0;
}
