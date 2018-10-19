#include <ros/ros.h>

#include <sensor_msgs/Imu.h>

#include <nav_msgs/Odometry.h>

#include <image_transport/image_transport.h>
#include <image_transport/subscriber_filter.h>
#include <message_filters/time_synchronizer.h>

#include <opencv2/opencv.hpp>
#include <cv_bridge/cv_bridge.h>

#include <iostream>
#include <cmath>

#include "percevite_pprzlink.hpp"
#include "percevite_messages.h"

// TODO http://wiki.ros.org/image_transport/Tutorials/SubscribingToImages
// TODO http://developer.parrot.com/docs/slamdunk/#code

// Goals:
// - Check depth map for:
//		- Distance to obstacle
//			- In what direction? Obtain vector from /pose or paparazzi?
//			- What if vector is out of view? -> Set 'safe distance' output to zero.
//		- Depth information available
//
// Planned program outline
// Initialization:
// 1. Initialize ROS
//		a. Subscribe to
//			- /depth_map/image
//			- /depth_map/camera_info -> for focal length
//		b. Advertise:
//			- /percevite/debug_image?
// 2. Use IMU to measure pitch relative to Bebop frame?
// 3. Output 'starting' status to paparazzi?
//
// Run:
// - Get movement direction. Options:
//		- From autopilot mission (e.g. waypoint in body frame)
//		- From autopilot INS
//		- From slamdunk odometry
//		- Or both plan and actual movement?
//
//		- If out of view: send yaw command to autopilot to center movement direction in image.
// - Get depth image
//		- Determine ROI from movement direction
//		- For pixels in depth image:
//			- Number of ok pixels (finite and inside image)

// Maybe: min-box-filter image, send highest-distance bearing to autopilot for alternative course?

namespace {

image_transport::Publisher pub_debug;

template <typename T>
T bound(const T& val, const T& min, const T& max) {
  if(val < min) return min;
  if(val > max) return max;
  return val;
}

struct slamdunk_orientation_t {
	ros::Subscriber sub;
	bool valid;
	int samples;
	double x;
	double y;
	double z;
	cv::Mat R_frd_cam; // Rotation from camera coordinates to front-right-down
	slamdunk_orientation_t(void) :
		valid(false),
		samples(0),
		x(0.0),
		y(0.0),
		z(0.0)
	{}
} slamdunk_orientation;

void on_imu(const sensor_msgs::Imu &imu) {
	if(slamdunk_orientation.samples < 100) {
		// Calibrate rotation
		slamdunk_orientation.x += imu.linear_acceleration.x / 100.0;
		slamdunk_orientation.y += imu.linear_acceleration.y / 100.0;
		slamdunk_orientation.z += imu.linear_acceleration.z / 100.0;
		++slamdunk_orientation.samples;
	} else {
		// Calibration finished
		ROS_INFO("Cam to body calibration complete");
		// Calculate R
		double &x = slamdunk_orientation.x;
		double &z = slamdunk_orientation.z;
		double norm = sqrt(x * x + z * z);
//		double R_frd_cam_data[] = {
//				0.0, -x / norm, -z / norm,
//				1.0,  0.0     ,  0.0     ,
//				0.0, -z / norm,  x / norm
//		};
    double R_frd_cam_data[] = {
        0.0, x / norm, z / norm,
        1.0,  0.0     ,  0.0     ,
        0.0, z / norm, -x / norm
    };
		slamdunk_orientation.R_frd_cam = cv::Mat(3, 3, CV_64F, R_frd_cam_data).clone();
		slamdunk_orientation.valid = true;
		ROS_INFO_STREAM("R_frd_cam = " << slamdunk_orientation.R_frd_cam);
		// Unsubscribe
		slamdunk_orientation.sub.shutdown();
	}
}

void on_odom(nav_msgs::Odometry odom) {
	if(!slamdunk_orientation.valid) return;
	// Transform to drone body frame
	cv::Mat vel(3, 1, CV_64F);
	vel.at<double>(0, 0) = odom.twist.twist.linear.x;
	vel.at<double>(1, 0) = odom.twist.twist.linear.y;
	vel.at<double>(2, 0) = odom.twist.twist.linear.z;
	vel = slamdunk_orientation.R_frd_cam * vel;
	// Output velocity to pprzlink
	SlamdunkToPaparazziMsg msg;
	msg.flags = SD_MSG_FLAG_VELOCITY;
	msg.vx = static_cast<float>(vel.at<double>(0, 0));
	msg.vy = static_cast<float>(vel.at<double>(1, 0));
	msg.vz = static_cast<float>(vel.at<double>(2, 0));
	pprzlink.write(sizeof(msg), &msg.bytes);
}

void on_image(
		const sensor_msgs::ImageConstPtr &color,
		const sensor_msgs::ImageConstPtr &depth) {
	cv_bridge::CvImageConstPtr cv_color_ptr = cv_bridge::toCvShare(color, "bgr8");
	cv_bridge::CvImageConstPtr cv_depth_ptr = cv_bridge::toCvShare(depth);

	// TODO Find relevant ROI (now takes entire region)
	cv::Rect roi(0, cv_depth_ptr->image.rows / 4,
			cv_depth_ptr->image.cols, cv_depth_ptr->image.rows / 2);
	cv::Mat depth_roi;
	cv_depth_ptr->image(roi).copyTo(depth_roi);

	// Calulate ROI stats
	int valid_pixels = 0;
	int distance_histogram[150];
	memset(distance_histogram, 0, sizeof(distance_histogram));
	for (int c = 0; c < depth_roi.cols; ++c) {
		for (int r = 0; r < depth_roi.rows; ++r) {
			if (std::isfinite(depth_roi.at<float>(r, c))) {
				++valid_pixels;
				int dist = depth_roi.at<float>(r, c) * 10.0;
				dist = dist < 0 ? 0 : dist;
				dist = dist > 149 ? 149 : dist;
				++(distance_histogram[dist]);
			}
		}
	}
	ROS_INFO("Valid pixels: %.0f %%",
			100.0 * valid_pixels / (depth_roi.cols * depth_roi.rows));
	int percentile = 5;
	int percentile_count = valid_pixels * percentile / 100;
	int dist;
	for(dist = 0; percentile_count > 0 && dist < 150; ++dist) {
		percentile_count -= distance_histogram[dist];
	}
	ROS_INFO("%d-percentile distance: %.1fm", percentile, dist / 10.0);
	// NOTE: For histogram, the discrete and limited disparities might be a better choice!

#ifdef DEBUG
	// ************************************************************************
	// For testing: highlight pixels smaller than dist in color image
	// ************************************************************************
	cv::Mat debug;
	cv_color_ptr->image.copyTo(debug);
	int ratio = debug.cols / cv_depth_ptr->image.cols;
	for(int x = 0; x < debug.cols; ++x) {
		for(int y = 0; y < debug.rows; ++y) {
			int x_roi = x / ratio;
			int y_roi = (y - debug.rows / 4) / ratio;
			bool inside_roi = (x_roi >= 0) && (x_roi < depth_roi.cols) &&
					(y_roi >= 0) && (y_roi < depth_roi.rows);
			if(inside_roi && depth_roi.at<float>(y_roi, x_roi) * 10.0 < dist) {
				cv::Vec3b color = debug.at<cv::Vec3b>(y, x);
				color.val[2] = 255;
				debug.at<cv::Vec3b>(y, x) = color;
			} else if (!inside_roi || !std::isfinite(depth_roi.at<float>(y_roi, x_roi))) {
				cv::Vec3b color = debug.at<cv::Vec3b>(y, x);
				color.val[0] = 255;
				debug.at<cv::Vec3b>(y, x) = color;
			}
		}
	}

	// OLD test code
	cv::imshow("Depth", depth_roi / 10.0);
	cv::imshow("Debug", debug);

	char key = cv::waitKey(1);
	if (key == 27 || key == 'q') {
		ros::shutdown();
	}
#endif

	// Send safe distance through pprzlink
	SlamdunkToPaparazziMsg msg;
	msg.flags = SD_MSG_FLAG_SAFE_DISTANCE;
	msg.safe_distance = (uint8_t)dist;
	msg.valid_pixels = (uint8_t)(255.0 * valid_pixels / (depth_roi.cols * depth_roi.rows));
	pprzlink.write(sizeof(msg), &msg.bytes);

	//**************************************************************************
	// For testing: read dummy text from pprzlink
	//**************************************************************************
	PaparazziToSlamdunkMsg msg2;
	if(pprzlink.read(sizeof(msg2), &msg2.bytes)) {
		ROS_INFO("Received request: tx = %f, ty = %f, tz = %f", msg2.tx, msg2.ty, msg2.tz);
	}

}

void on_cspace(const sensor_msgs::ImageConstPtr &cspace_msg,
    const sensor_msgs::ImageConstPtr &color_msg) {
  const float F = 425.0 / 6.0; // TODO REMOVE. C-Space map x,y focal length
  const float F_disp = 425.0; // C-Space z focal length
  const float B = 0.20; // Baseline
  const int ndisp = 64;

  int debug_xq = 999.0; // TODO clean up
  int debug_yq = 999.0;

  cv::Mat_<float> cspace(cspace_msg->height, cspace_msg->width,
      (float*)(&(cspace_msg->data[0])));
  cv_bridge::CvImageConstPtr cv_color_ptr = cv_bridge::toCvShare(color_msg, "bgr8");
  cv::Mat color = cv_color_ptr->image;

  PaparazziToSlamdunkMsg request_msg;
  if(pprzlink.read(sizeof(request_msg), &request_msg.bytes)) {
    ROS_INFO("Request (FRD): tx = %f, ty = %f, tz = %f",
        request_msg.tx, request_msg.ty, request_msg.tz);

    cv::Mat_<double> request_frd(3, 1);
    request_frd << request_msg.tx, request_msg.ty, request_msg.tz;
//    ROS_INFO_STREAM("request_frd " << request_frd);
    cv::Mat_<double> request(slamdunk_orientation.R_frd_cam.t() * request_frd);
//    ROS_INFO_STREAM("Request (CAM VEC) " << request);
    double rx = request(0, 0);
    double ry = request(1, 0);
    double rz = request(2, 0);

//    ROS_INFO("Request (CAM): tx = %f, ty = %f, tz = %f", rx, ry, rz);

    int xq = cspace.cols / 2.0 + rx / rz * F;
    int yq = cspace.rows / 2.0 + ry / rz * F;
    if(rz > 0) {
      debug_xq = xq * F_disp / F;
      debug_yq = yq * F_disp / F;
    }

    double x, y, z;
    if(rz > 0 && xq >= 0 && xq < cspace.cols && yq >= 0 && yq < cspace.rows) {
      double d = cspace(yq, xq);
      if(d < (ndisp - 1)) {
        z = F_disp * B / d;
      } else { // Inside safety radius
        z = 0.0;
      }
      if(z > rz) z = rz; // Do not move past goal
      x = rx / rz * z;
      y = ry / rz * z;
    } else {
      x = 0.0; // Not in view, so can't guarantee safety
      y = 0.0;
      z = 0.0;
    }

    cv::Mat_<double> reply(3, 1);
    reply << x, y, z;
    cv::Mat_<double> reply_frd(slamdunk_orientation.R_frd_cam * reply);
    rx = reply_frd(0, 0);
    ry = reply_frd(1, 0);
    rz = reply_frd(2, 0);

    ROS_INFO("Reply (FRD): rx = %f, ry = %f, rz = %f", rx, ry, rz);

    // TODO Send to pprz
  }

  if(pub_debug.getNumSubscribers() > 0) {
    cv::Mat_<uint8_t> cspace_u8;
    cv::Mat_<cv::Vec3b> cspace_rgb;
    cv::Mat debug;

    // Overlay cspace and color images
    cspace.convertTo(cspace_u8, CV_8U, 255.0 / (double)ndisp);
    cv::applyColorMap(cspace_u8, cspace_rgb, cv::COLORMAP_JET);
    cv::Mat nan_mask(cspace != cspace);
    cspace_rgb.setTo(cv::Vec3b(255, 255, 255), nan_mask);
    cv::resize(cspace_rgb, cspace_rgb, color.size(), 0, 0, CV_INTER_NN);
    cv::addWeighted(cspace_rgb, 0.5, color, 0.5, 0, debug);

    // Draw goal marker
    if(debug_xq != 999.0 || debug_yq != 999.0) {
      // Color 255 165 0 (RGB)
      cv::Point_<int> points[] = {
          cv::Point_<int>(debug_xq - 10.0, debug_yq - 10.0),
          cv::Point_<int>(debug_xq + 10.0, debug_yq - 10.0),
          cv::Point_<int>(debug_xq, debug_yq + 10.0)
      };
      cv::fillConvexPoly(debug, points, 3, cv::Vec3b(0, 165, 255));
    }

    sensor_msgs::ImagePtr debug_msg = cv_bridge::CvImage(cspace_msg->header,
        sensor_msgs::image_encodings::BGR8, debug).toImageMsg();
    pub_debug.publish(debug_msg);
  }
}

} // namespace

int main(int argc, char **argv) {
	ros::init(argc, argv, "percevite");
	ros::NodeHandle nh;
	ros::NodeHandle nh_private("~");

	// Subscribe to IMU
	slamdunk_orientation.sub = nh.subscribe("/imu", 100, &on_imu);

	// Subscribe to odom
	ros::Subscriber odom_sub = nh.subscribe("/odom", 100, &on_odom);

//	// Subscribe to two image topics
//	image_transport::ImageTransport it(nh);
//	image_transport::SubscriberFilter sub_depth(it, "/depth_map/image", 3);
//	image_transport::SubscriberFilter sub_color(it,
//			"/left_rgb_rect/image_rect_color", 10);
//
//	// Synchronize the two topics
//	message_filters::TimeSynchronizer<sensor_msgs::Image, sensor_msgs::Image> sync(
//			sub_color, sub_depth, 10);
//	sync.registerCallback(boost::bind(&on_image, _1, _2));

	// Subscribe to cspace
	image_transport::ImageTransport it(nh);
	image_transport::SubscriberFilter sf_cspace(it, "/cspace_map/image", 1);
	image_transport::SubscriberFilter sf_color(it, "/left_rgb_rect/image_rect_color", 1);
	message_filters::TimeSynchronizer<sensor_msgs::Image, sensor_msgs::Image>
	    sync(sf_cspace, sf_color, 1);
	sync.registerCallback(on_cspace);

	// Advertise debug image
	image_transport::ImageTransport it_private(nh_private);
	pub_debug = it_private.advertise("debug_image", 1);

	// Initialize pprzlink
	pprzlink.init();

	ros::spin();

	return 0;
}
