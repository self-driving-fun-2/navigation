/*
* Copyright (C)2015  iCub Facility - Istituto Italiano di Tecnologia
* Author: Marco Randazzo
* email:  marco.randazzo@iit.it
* website: www.robotcub.org
* Permission is granted to copy, distribute, and/or modify this program
* under the terms of the GNU General Public License, version 2 or any
* later version published by the Free Software Foundation.
*
* A copy of the license can be found at
* http://www.robotcub.org/icub/license/gpl.txt
*
* This program is distributed in the hope that it will be useful, but
* WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
* Public License for more details
*/

#include "odometry.h"
#include <yarp/os/LogStream.h>
#include <limits>

#define RAD2DEG 180.0/M_PI
#define DEG2RAD M_PI/180.0

inline yarp::rosmsg::TickTime normalizeSecNSec(double yarpTimeStamp)
{
    uint64_t time = (uint64_t)(yarpTimeStamp * 1000000000UL);
    uint64_t nsec_part = (time % 1000000000UL);
    uint64_t sec_part = (time / 1000000000UL);
    yarp::rosmsg::TickTime ret;

    if (sec_part > std::numeric_limits<unsigned int>::max())
    {
        yWarning() << "Timestamp exceeded the 64 bit representation, resetting it to 0";
        sec_part = 0;
    }

    ret.sec = (yarp::os::NetUint32) sec_part;
    ret.nsec = (yarp::os::NetUint32) nsec_part;
    return ret;
}

Odometry::~Odometry()
{
    close();
}

Odometry::Odometry(PolyDriver* _driver)
{
    control_board_driver = _driver;
    odom_x               = 0;
    odom_y               = 0;
    odom_z               = 0;
    odom_theta           = 0;
    odom_vel_x           = 0;
    odom_vel_y           = 0;
    odom_vel_lin         = 0;
    odom_vel_theta       = 0;
    base_vel_x           = 0;
    base_vel_y           = 0;
    base_vel_lin         = 0;
    base_vel_theta       = 0;
    traveled_distance    = 0;
    traveled_angle       = 0;
    rosMsgCounter        = 0;
    odometry_portname_suffix = "/odometry:o";
    odometer_portname_suffix = "/odometer:o";
    velocity_portname_suffix = "/velocity:o";
}

bool Odometry::open(Property &options)
{
    ctrl_options = options;
    localName = ctrl_options.find("local").asString();

    if (ctrl_options.check("odometry_portname_suffix"))
    {
        odometry_portname_suffix = ctrl_options.find("odometry_portname_suffix").asString();
    }
    if (ctrl_options.check("odometer_portname_suffix"))
    {
        odometer_portname_suffix = ctrl_options.find("odometer_portname_suffix").asString();
    }
    if (ctrl_options.check("velocity_portname_suffix"))
    {
        velocity_portname_suffix = ctrl_options.find("velocity_portname_suffix").asString();
    }

    // open the control board driver
    yInfo("Opening the motors interface...");

    Property control_board_options("(device remote_controlboard)");
    if (!control_board_driver)
    {
        yError("control board driver not ready!");
        return false;
    }
    // open the interfaces for the control boards
    bool ok = true;
    ok = ok & control_board_driver->view(ienc);
    if (!ok)
    {
        yError("one or more devices has not been viewed");
        return false;
    }
    // open control input ports
    bool ret = true;
    ret &= port_odometry.open((localName + odometry_portname_suffix).c_str());
    ret &= port_odometer.open((localName + odometer_portname_suffix).c_str());
    ret &= port_vels.open((localName + velocity_portname_suffix).c_str());
    if (ret == false)
    {
        yError() << "Unable to open module ports";
        return false;
    }

    //reset odometry
    reset_odometry();

    //the ROS part
    if (ctrl_options.check("GENERAL"))
    {
        yarp::os::Bottle g_group = ctrl_options.findGroup("GENERAL");
        enable_ROS = (g_group.find("use_ROS").asBool() == true);
        if (enable_ROS) yInfo() << "ROS enabled";
        else
            yInfo() << "ROS not enabled";
    }
    else
    {
        yError() << "Missing [GENERAL] section";
        return false;
    }

    if (enable_ROS)
    {
        if (ctrl_options.check("ROS_ODOMETRY"))
        {
            yarp::os::Bottle ro_group = ctrl_options.findGroup("ROS_ODOMETRY");
            if (ro_group.check("odom_frame") == false) { yError() << "Missing odom_frame parameter"; return false; }
            if (ro_group.check("base_frame") == false) { yError() << "Missing base_frame parameter"; return false; }
            if (ro_group.check("topic_name") == false) { yError() << "Missing topic_name parameter"; return false; }
            odometry_frame_id = ro_group.find("odom_frame").asString();
            child_frame_id = ro_group.find("base_frame").asString();
            rosTopicName_odometry = ro_group.find("topic_name").asString();
        }
        else
        {
            yError() << "Missing [ROS_ODOMETRY] section";
            return false;
        }
    }

    if (enable_ROS)
    {
        if (ctrl_options.check("ROS_FOOTPRINT"))
        {
            yarp::os::Bottle rf_group = ctrl_options.findGroup("ROS_FOOTPRINT");
            if (rf_group.check("topic_name") == false) { yError() << "Missing topic_name parameter"; return false; }
            if (rf_group.check("footprint_diameter") == false) { yError() << "Missing footprint_diameter parameter"; return false; }
            if (rf_group.check("footprint_frame") == false) { yError() << "Missing footprint_frame parameter"; return false; }
            footprint_frame_id = rf_group.find("footprint_frame").asString();
            rosTopicName_footprint = rf_group.find("topic_name").asString();
            footprint_diameter = rf_group.find("footprint_diameter").asDouble();
        }
        else
        {
            yError() << "Missing [ROS_FOOTPRINT] section";
            return false;
        }
    }

    if (enable_ROS)
    {

        if (!rosPublisherPort_odometry.topic(rosTopicName_odometry))
        {
            yError() << " opening " << rosTopicName_odometry << " Topic, check your yarp-ROS network configuration\n";
            return false;
        }

        if (!rosPublisherPort_footprint.topic(rosTopicName_footprint))
        {
            yError() << " opening " << rosTopicName_footprint << " Topic, check your yarp-ROS network configuration\n";
            return false;
        }

        if (!rosPublisherPort_tf.topic("/tf"))
        {
            yError() << " opening " << "/tf" << " Topic, check your yarp-ROS network configuration\n";
            return false;
        }

        footprint.polygon.points.resize(12);
        double r = footprint_diameter;
        for (int i = 0; i< 12; i++)
        {
            double t = M_PI * 2 / 12 * i;
            footprint.polygon.points[i].x = (yarp::os::NetFloat32) (r*cos(t));
            footprint.polygon.points[i].y = (yarp::os::NetFloat32) (r*sin(t));
            footprint.polygon.points[i].z = 0;
        }
    }
    return true;
}

void Odometry::close()
{
    port_odometry.interrupt();
    port_odometry.close();
    port_odometer.interrupt();
    port_odometer.close();
    port_vels.interrupt();
    port_vels.close();

    if (enable_ROS)
    {
        rosPublisherPort_footprint.interrupt();
        rosPublisherPort_footprint.close();
        rosPublisherPort_odometry.interrupt();
        rosPublisherPort_odometry.close();
        rosPublisherPort_tf.interrupt();
        rosPublisherPort_tf.close();
    }
}

void Odometry::broadcast()
{
    mutex.wait();
    timeStamp.update();
    if (port_odometry.getOutputCount()>0)
    {
        port_odometry.setEnvelope(timeStamp);
        Bottle &b = port_odometry.prepare();
        b.clear();
        b.addDouble(odom_x); //position in the odom reference frame
        b.addDouble(odom_y);
        b.addDouble(odom_theta);
        b.addDouble(base_vel_x); //velocity in the robot reference frame
        b.addDouble(base_vel_y);
        b.addDouble(base_vel_theta);
        b.addDouble(odom_vel_x); //velocity in the odom reference frame
        b.addDouble(odom_vel_y);
        b.addDouble(odom_vel_theta);
        port_odometry.write();
    }

    if (port_odometer.getOutputCount()>0)
    {
        port_odometer.setEnvelope(timeStamp);
        Bottle &t = port_odometer.prepare();
        t.clear();
        t.addDouble(traveled_distance);
        t.addDouble(traveled_angle);
        port_odometer.write();
    }

    if (port_vels.getOutputCount()>0)
    {
        port_vels.setEnvelope(timeStamp);
        Bottle &v = port_vels.prepare();
        v.clear();
        v.addDouble(base_vel_lin);
        v.addDouble(base_vel_theta);
        port_vels.write();
    }

    if (enable_ROS)
    {
        yarp::rosmsg::nav_msgs::Odometry &rosData = rosPublisherPort_odometry.prepare();
        rosData.header.seq = rosMsgCounter;
        rosData.header.stamp = normalizeSecNSec(yarp::os::Time::now());
        rosData.header.frame_id = odometry_frame_id;
        rosData.child_frame_id = child_frame_id;

        rosData.pose.pose.position.x = odom_x;
        rosData.pose.pose.position.y = odom_y;
        rosData.pose.pose.position.z = 0.0;
        yarp::rosmsg::geometry_msgs::Quaternion odom_quat;
        double halfYaw = odom_theta / 180.0*M_PI * 0.5;
        double cosYaw = cos(halfYaw);
        double sinYaw = sin(halfYaw);
        odom_quat.x = 0;
        odom_quat.y = 0;
        odom_quat.z = sinYaw;
        odom_quat.w = cosYaw;
        rosData.pose.pose.orientation = odom_quat;
        rosData.twist.twist.linear.x = base_vel_x;
        rosData.twist.twist.linear.y = base_vel_y;
        rosData.twist.twist.linear.z = 0;
        rosData.twist.twist.angular.x = 0;
        rosData.twist.twist.angular.y = 0;
        rosData.twist.twist.angular.z = base_vel_theta / 180.0*M_PI;

        rosPublisherPort_odometry.write();
    }

    if (enable_ROS)
    {
        yarp::rosmsg::geometry_msgs::PolygonStamped &rosData = rosPublisherPort_footprint.prepare();
        rosData = footprint;
        rosData.header.seq = rosMsgCounter;
        rosData.header.stamp = normalizeSecNSec(yarp::os::Time::now());
        rosData.header.frame_id = footprint_frame_id;
        rosPublisherPort_footprint.write();
    }

    if (enable_ROS)
    {
        yarp::rosmsg::tf2_msgs::TFMessage &rosData = rosPublisherPort_tf.prepare();
        yarp::rosmsg::geometry_msgs::TransformStamped transform;
        transform.child_frame_id = child_frame_id;
        transform.header.frame_id = odometry_frame_id;
        transform.header.seq = rosMsgCounter;
        transform.header.stamp = normalizeSecNSec(yarp::os::Time::now());
        double halfYaw = odom_theta / 180.0*M_PI * 0.5;
        double cosYaw = cos(halfYaw);
        double sinYaw = sin(halfYaw);
        transform.transform.rotation.x = 0;
        transform.transform.rotation.y = 0;
        transform.transform.rotation.z = sinYaw;
        transform.transform.rotation.w = cosYaw;
        transform.transform.translation.x = odom_x;
        transform.transform.translation.y = odom_y;
        transform.transform.translation.z = odom_z;
        if (rosData.transforms.size() == 0)
        {
            rosData.transforms.push_back(transform);
        }
        else
        {
            rosData.transforms[0] = transform;
        }


        rosPublisherPort_tf.write();
    }

    rosMsgCounter++;

    mutex.post();
}

double Odometry::get_base_vel_lin()
{
    return this->base_vel_lin;
}

double Odometry::get_base_vel_theta()
{
    return this->base_vel_theta;
}
