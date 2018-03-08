#include <ros/ros.h>
#include <simple_velocity_controller/multi_segment_controller_node.h>
#include <tf/transform_datatypes.h>
#include <string>
#include <algorithm>

#define NSEC_2_SECS(A)      ((float)A/1000000000.0)


int main(int argc, char** argv)
{
    if(argc >= 2)
    {
        ros::init(argc, argv, "controller");     /// initializes the ros node with default name
        ros::NodeHandle n;

        velocity_controller::MultiSegmentControllerNode ctrl(n);
        ros::Rate r(20);

        while(ros::ok())
        {
            ros::spinOnce();
        }

        return 0;
    }
    else
    {
        ROS_INFO("Please specifie name \nrosrun simple_velocity_controller velocity_controller [name]");
    }
}


namespace velocity_controller
{
    MultiSegmentControllerNode::MultiSegmentControllerNode(ros::NodeHandle& n) :
        n_(n),
        n_param_("~"),
        robot_names_(std::vector<std::string> ( {"robot0"}))
    {
        n_param_.param("robot_names", robot_names_, std::vector<std::string>());

        std::string robot_names_string = "";
        n_param_.param("robot_names_str", robot_names_string, robot_names_string);

        if(robot_names_string.size() > 0)
        {
            robot_names_string.erase(std::remove(robot_names_string.begin(), robot_names_string.end(), ' '), robot_names_string.end());
            std::istringstream stringStr(robot_names_string);
            std::string result;

            robot_names_.clear();
            while(std::getline(stringStr, result, ','))
            {
                robot_names_.push_back(result);
            }
        }

        controller.resize(robot_names_.size());
        pubCmdVel_.resize(robot_names_.size());
        subCtrl_.resize(robot_names_.size());
        subOdom_.resize(robot_names_.size());
        subPath_.resize(robot_names_.size());

        topic_odom_ = "odom";
        n.getParam("odom_topic", topic_odom_);

        topic_cmdVel_ = "cmd_vel";
        n.getParam("cmd_vel_topic", topic_cmdVel_);

        topic_path_ = "seg_path";
        n.getParam("path_topic", topic_path_);

        max_vel_v_ = 0.8;
        n.getParam("max_v", max_vel_v_);

        max_vel_w_ = 1.0;
        n.getParam("max_w", max_vel_w_);

        goal_r_ = 0.2;
        n.getParam("goal_radius", goal_r_);

        Kp_val_ = 5.0;
        n.getParam("Kp", Kp_val_);

        Ki_val_ = 0.0;
        n.getParam("Ki", Ki_val_);

        Kd_val_ = 1.0;
        n.getParam("Kd", Kd_val_);

        topic_ctrl_ = "/ctrl";
        n.getParam("topic_control", topic_ctrl_);

        ROS_INFO("Multi Robot Controller:  %s", topic_cmdVel_.c_str());

        for(auto & ctrl : controller)
        {
            ctrl.setSpeedParams(max_vel_v_, max_vel_w_);
            ctrl.setPID(Kp_val_, Ki_val_, Kd_val_);
            ctrl.setGoalRadius(goal_r_);
        }

        for(int i = 0; i < robot_names_.size(); i++)
        {
            pubCmdVel_[i] = n.advertise<geometry_msgs::Twist>(robot_names_[i] + "/" + topic_cmdVel_, 1);
            subOdom_[i] = n.subscribe<nav_msgs::Odometry> (robot_names_[i] + "/" + topic_odom_, 1, boost::bind(&MultiSegmentControllerNode::subOdomCb, this, _1, i));
            subPath_[i] = n.subscribe<tuw_multi_robot_msgs::SegmentPath> (robot_names_[i] + "/" + topic_path_, 1, boost::bind(&MultiSegmentControllerNode::subPathCb, this, _1, i));
            subCtrl_[i] = n.subscribe<std_msgs::String> (robot_names_[i] + "/" + topic_ctrl_, 1, boost::bind(&MultiSegmentControllerNode::subCtrlCb, this, _1, i));
        }
    }

    void velocity_controller::MultiSegmentControllerNode::subOdomCb(const ros::MessageEvent< const nav_msgs::Odometry >& _event, int _topic)
    {
        const nav_msgs::Odometry_< std::allocator< void > >::ConstPtr& odom = _event.getMessage();

        PathPoint pt;
        pt.x = odom->pose.pose.position.x;
        pt.y = odom->pose.pose.position.y;

        tf::Quaternion q(odom->pose.pose.orientation.x, odom->pose.pose.orientation.y, odom->pose.pose.orientation.z, odom->pose.pose.orientation.w);
        tf::Matrix3x3 m(q);
        double roll, pitch, yaw;
        m.getRPY(roll, pitch, yaw);

        pt.theta = yaw;

        ros::Time time = ros::Time::now();
        ros::Duration d = time - last_update_;

        float delta_t = d.sec + NSEC_2_SECS(d.nsec);
        controller[_topic].update(pt, delta_t);

        geometry_msgs::Twist msg;

        float v, w;
        controller[_topic].getSpeed(&v, &w);
        msg.linear.x = v;
        msg.angular.z = w;

        pubCmdVel_[_topic].publish(msg);



        //Update
        PathPrecondition pc = {_topic, controller[_topic].getCount()};

        for(SegmentController & c : controller)
        {
            c.updatePrecondition(pc);
        }
    }

    void velocity_controller::MultiSegmentControllerNode::subPathCb(const ros::MessageEvent< const tuw_multi_robot_msgs::SegmentPath >& _event, int _topic)
    {
        const tuw_multi_robot_msgs::SegmentPath_< std::allocator< void > >::ConstPtr& path = _event.getMessage();

        std::vector<PathPoint> localPath;

        if(path->poses.size() == 0)
            return;


        for(const tuw_multi_robot_msgs::PathSegment & seg : path->poses)
        {
            PathPoint pt;

            pt.x = seg.end.x;
            pt.y = seg.end.y;
            pt.theta = 0;

            for(const tuw_multi_robot_msgs::PathPrecondition & pc : seg.preconditions)
            {
                PathPrecondition p;
                p.robot = pc.robotId;
                p.stepCondition = pc.stepCondition;
                pt.precondition.push_back(p);
            }

            localPath.push_back(pt);
        }

        controller[_topic].setPath(std::make_shared<std::vector<PathPoint>>(localPath));
        ROS_INFO("Multi Robot Controller: Got Plan");
    }

    void velocity_controller::MultiSegmentControllerNode::subCtrlCb(const ros::MessageEvent< const std_msgs::String >& _event, int _topic)
    {
        const std_msgs::String_< std::allocator< void > >::ConstPtr& cmd = _event.getMessage();
        std::string s = cmd->data;

        ROS_INFO("Multi Robot Controller: received %s", s.c_str());

        if(s.compare("run") == 0)
        {
            controller[_topic].setState(run);
        }
        else if(s.compare("stop") == 0)
        {
            controller[_topic].setState(stop);
        }
        else if(s.compare("step") == 0)
        {
            controller[_topic].setState(step);
        }
        else
        {
            controller[_topic].setState(run);
        }
    }
}


