/*******************************************************
 * Copyright (C) 2019, Aerial Robotics Group, Hong Kong University of Science and Technology
 * 
 * This file is part of VINS.
 * 
 * Licensed under the GNU General Public License v3.0;
 * you may not use this file except in compliance with the License.
 *
 * Author: Qin Tong (qintonguav@gmail.com)
 *******************************************************/

#include "ros/ros.h"
#include "globalOpt.h"
#include <sensor_msgs/NavSatFix.h>
#include <nav_msgs/Odometry.h>
#include <nav_msgs/Path.h>
#include <eigen3/Eigen/Dense>
#include <eigen3/Eigen/Geometry>
#include <iostream>
#include <stdio.h>
#include <visualization_msgs/Marker.h>
#include <visualization_msgs/MarkerArray.h>
#include <fstream>
#include <queue>
#include <mutex>
#include <vector>
#include <string>
#include <ctime>

GlobalOptimization globalEstimator;
ros::Publisher pub_global_odometry, pub_global_path, pub_car, pub_ground_truth, pub_adjusted_global_path;
nav_msgs::Path *global_path;
nav_msgs::Path *adjusted_global_path;
nav_msgs::Path truth_path;
std::string saving_path;

// shared time variable
double shared_time;

// ground truth publishing
geometry_msgs::PoseStamped gt_pose_stamped;

double last_vio_t = -1;
std::queue<sensor_msgs::NavSatFixConstPtr> gpsQueue;
std::mutex m_buf;

void publish_car_model(double t, Eigen::Vector3d t_w_car, Eigen::Quaterniond q_w_car)
{
    visualization_msgs::MarkerArray markerArray_msg;
    visualization_msgs::Marker car_mesh;
    car_mesh.header.stamp = ros::Time(t);
    car_mesh.header.frame_id = "world";
    car_mesh.type = visualization_msgs::Marker::MESH_RESOURCE;
    car_mesh.action = visualization_msgs::Marker::ADD;
    car_mesh.id = 0;

    car_mesh.mesh_resource = "package://global_fusion/models/car.dae";

    Eigen::Matrix3d rot;
    rot << 0, 0, -1, 0, -1, 0, -1, 0, 0;
    
    Eigen::Quaterniond Q;
    Q = q_w_car * rot; 
    car_mesh.pose.position.x    = t_w_car.x();
    car_mesh.pose.position.y    = t_w_car.y();
    car_mesh.pose.position.z    = t_w_car.z();
    car_mesh.pose.orientation.w = Q.w();
    car_mesh.pose.orientation.x = Q.x();
    car_mesh.pose.orientation.y = Q.y();
    car_mesh.pose.orientation.z = Q.z();

    car_mesh.color.a = 1.0;
    car_mesh.color.r = 1.0;
    car_mesh.color.g = 0.0;
    car_mesh.color.b = 0.0;

    float major_scale = 2.0;

    car_mesh.scale.x = major_scale;
    car_mesh.scale.y = major_scale;
    car_mesh.scale.z = major_scale;
    markerArray_msg.markers.push_back(car_mesh);
    pub_car.publish(markerArray_msg);
}

void GPS_callback(const sensor_msgs::NavSatFixConstPtr &GPS_msg)
{
    //printf("gps_callback! \n");
    m_buf.lock();
    gpsQueue.push(GPS_msg);
    m_buf.unlock();
}

void vio_callback(const nav_msgs::Odometry::ConstPtr &pose_msg)
{
    //printf("vio_callback! \n");
    double t = pose_msg->header.stamp.toSec();
    last_vio_t = t;
    // set shared time
    shared_time = t;
    Eigen::Vector3d vio_t(pose_msg->pose.pose.position.x, pose_msg->pose.pose.position.y, pose_msg->pose.pose.position.z);
    Eigen::Quaterniond vio_q;
    vio_q.w() = pose_msg->pose.pose.orientation.w;
    vio_q.x() = pose_msg->pose.pose.orientation.x;
    vio_q.y() = pose_msg->pose.pose.orientation.y;
    vio_q.z() = pose_msg->pose.pose.orientation.z;
    globalEstimator.inputOdom(t, vio_t, vio_q);


    m_buf.lock();
    while(!gpsQueue.empty())
    {
        sensor_msgs::NavSatFixConstPtr GPS_msg = gpsQueue.front();
        double gps_t = GPS_msg->header.stamp.toSec();
        // printf("vio t: %f, gps t: %f \n", t, gps_t);
        // 10ms sync tolerance
        if(gps_t >= t - 0.01 && gps_t <= t + 0.01)
        {
            printf("receive GPS with timestamp %f\n", GPS_msg->header.stamp.toSec());
            double latitude = GPS_msg->latitude;
            double longitude = GPS_msg->longitude;
            double altitude = GPS_msg->altitude;
            //int numSats = GPS_msg->status.service;
            double pos_accuracy = GPS_msg->position_covariance[0];
            if(pos_accuracy <= 0)
                pos_accuracy = 1;
            //printf("receive covariance %lf \n", pos_accuracy);
            //if(GPS_msg->status.status > 8)
                globalEstimator.inputGPS(t, latitude, longitude, altitude, pos_accuracy);
            gpsQueue.pop();
            break;
        }
        else if(gps_t < t - 0.01)
            gpsQueue.pop();
        else if(gps_t > t + 0.01)
            break;
    }
    m_buf.unlock();

    Eigen::Vector3d global_t;
    Eigen:: Quaterniond global_q;
    globalEstimator.getGlobalOdom(global_t, global_q);

    nav_msgs::Odometry odometry;
    odometry.header = pose_msg->header;
    odometry.header.frame_id = "world";
    odometry.child_frame_id = "world";
    odometry.pose.pose.position.x = global_t.x();
    odometry.pose.pose.position.y = global_t.y();
    odometry.pose.pose.position.z = global_t.z();
    odometry.pose.pose.orientation.x = global_q.x();
    odometry.pose.pose.orientation.y = global_q.y();
    odometry.pose.pose.orientation.z = global_q.z();
    odometry.pose.pose.orientation.w = global_q.w();
    pub_global_odometry.publish(odometry);
    pub_global_path.publish(*global_path);
    // sync ground truth
    pub_ground_truth.publish(truth_path);
    // publish adjusted global path
    pub_adjusted_global_path.publish(*adjusted_global_path);
    
    publish_car_model(t, global_t, global_q);
}

void truth_callback(const nav_msgs::Odometry::ConstPtr &pose_msg)
{
    gt_pose_stamped.header.stamp = pose_msg->header.stamp;
    gt_pose_stamped.header.frame_id = "world";
    gt_pose_stamped.pose.position.x = pose_msg->pose.pose.position.x;
    gt_pose_stamped.pose.position.y = pose_msg->pose.pose.position.y;
    gt_pose_stamped.pose.position.z = pose_msg->pose.pose.position.z;
    gt_pose_stamped.pose.orientation.x = pose_msg->pose.pose.orientation.x;
    gt_pose_stamped.pose.orientation.y = pose_msg->pose.pose.orientation.y;
    gt_pose_stamped.pose.orientation.z = pose_msg->pose.pose.orientation.z;
    gt_pose_stamped.pose.orientation.w = pose_msg->pose.pose.orientation.w;
    truth_path.header = gt_pose_stamped.header;
    truth_path.poses.push_back(gt_pose_stamped);
    globalEstimator.start_x = truth_path.poses[0].pose.position.x;
    globalEstimator.start_y = truth_path.poses[0].pose.position.y;
    globalEstimator.start_z = truth_path.poses[0].pose.position.z;
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, "globalEstimator");
    ros::NodeHandle n("~");

    global_path = &globalEstimator.global_path;
    adjusted_global_path = &globalEstimator.adjusted_global_path;

    // GPS topic
    ros::Subscriber sub_GPS = n.subscribe("/gps", 100, GPS_callback);
    // VIO topic
    ros::Subscriber sub_vio = n.subscribe("/vins_estimator/odometry", 100, vio_callback);
    // Ground Truth topic
    ros::Subscriber sub_truth = n.subscribe("/ground_truth", 0, truth_callback);

    // global path publisher
    pub_global_path = n.advertise<nav_msgs::Path>("global_path", 100);
    // ground truth path publisher
    pub_ground_truth = n.advertise<nav_msgs::Path>("ground_truth_path", 100);
    // adjusted global path publisher
    pub_adjusted_global_path = n.advertise<nav_msgs::Path>("adjusted_global_path", 100);

    // global odometry
    pub_global_odometry = n.advertise<nav_msgs::Odometry>("global_odometry", 100);
    pub_car = n.advertise<visualization_msgs::MarkerArray>("car_model", 1000);
    ros::spin();

    // create GPS timestamps
     vector<double> timestamp_vector;
     double timestamp;
     time_t current_time = time(NULL);

    if (ros::param::get("saving_path", saving_path))
    {
        std::ofstream foutC(saving_path + "estimation_" + std::to_string(current_time) + ".csv", ios::app);
        foutC.setf(ios::fixed, ios::floatfield);
        for (long unsigned int i = 0; i < adjusted_global_path->poses.size(); i++){
            foutC.precision(0);
            timestamp = adjusted_global_path->poses[i].header.stamp.toSec() * 1e9;
            timestamp_vector.push_back(timestamp);
            foutC << timestamp << ",";
            foutC.precision(5);
            foutC << adjusted_global_path->poses[i].pose.position.x << ","
                    << adjusted_global_path->poses[i].pose.position.y << ","
                    << adjusted_global_path->poses[i].pose.position.z  << endl;
        }
        foutC.close();


        std::ofstream foutD(saving_path + "validation_" + std::to_string(current_time) + ".csv", ios::app);
        foutD.setf(ios::fixed, ios::floatfield);
        for (long unsigned int i = 0; i < truth_path.poses.size(); i++){
            foutD.precision(0);
            timestamp = truth_path.poses[i].header.stamp.toSec() * 1e9;
            if (std::find(timestamp_vector.begin(), timestamp_vector.end(), timestamp) != timestamp_vector.end()) {
                foutD << timestamp << ",";
                foutD.precision(5);
                foutD << truth_path.poses[i].pose.position.x << ","
                        << truth_path.poses[i].pose.position.y << ","
                        << truth_path.poses[i].pose.position.z  << endl;
            }
        }
        foutD.close();
    }
    return 0;
}
