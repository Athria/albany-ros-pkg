/**

\author Michael Ferguson

@b Publish a transform between the checkerboard and the camera link. 

**/

#include <iostream>
#include <algorithm>
#include <limits>
#include <math.h>

#include <ros/ros.h>

#include <sensor_msgs/Image.h>
#include <sensor_msgs/CameraInfo.h>
#include <sensor_msgs/PointCloud2.h>

#include <opencv/cv.h>
#include <opencv/highgui.h>
#include <cv_bridge/cv_bridge.h>
#include <tf/transform_broadcaster.h>

#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>

#include <pcl/io/io.h>
#include <pcl/point_types.h>
#include <pcl/registration/icp.h>
#include <pcl/registration/registration.h>

#include <pcl_ros/point_cloud.h>
#include <pcl_ros/transforms.h>

using namespace std;
typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::Image, sensor_msgs::PointCloud2> CameraSyncPolicy;
typedef pcl::PointXYZ point;
typedef pcl::PointXYZRGB color_point;

/** @brief Helper function to find intersection of two lines */ 
cv::Point findIntersection( cv::Vec4i a, cv::Vec4i b )
{
    double ma = (a[3]-a[1])/(double)(a[2]-a[0]);
    double mb = (b[3]-b[1])/(double)(b[2]-b[0]);
    double ba = a[1] - ma*a[0];
    double bb = b[1] - mb*b[0];
    
    double x = (bb-ba)/(ma-mb);
    double y = ma*x + ba;

    if( (x>=0) && (x<640) && (y>=0) && (y<480) ){
        return cv::Point((int)x,(int)y);
    }else{
        return cv::Point(-1,-1);
    }
}

/** @brief Helper function to convert Eigen transformation to tf -- thanks to Garret Gallagher */
tf::Transform tfFromEigen(Eigen::Matrix4f trans)
{
    btMatrix3x3 btm;
    btm.setValue(trans(0,0),trans(0,1),trans(0,2),
               trans(1,0),trans(1,1),trans(1,2),
               trans(2,0),trans(2,1),trans(2,2));
    btTransform ret;
    ret.setOrigin(btVector3(trans(0,3),trans(1,3),trans(2,3)));
    ret.setBasis(btm);
    return ret;
}

/** @brief A class for locating the chess board and publishing a transform. 
  * Detection proceeds as follows--
  *   1)  RGB image is converted a grayscale using the blue-channel only. 
  *   2)  We threshold the image and then run a Canny edge detector, 
  *         various dilations/erosions are used to improve performance. 
  *   3)  We perform a hough transformation to find lines.
  *   4)  We split lines into horizontal/vertical groups.
  *   5)  We find intersections between lines in the horizontal/vertical groups. 
  *   6)  Each intersection point is then converted to the corresponding point
  *         in the point cloud (x,y,z).  
  *   7) We iterate through possible orientations, finding best fit. 
  */
class ChessBoardLocator
{
  public:
    ChessBoardLocator(ros::NodeHandle & n): nh_ (n),
        image_sub_ (nh_, "/camera/rgb/image_color", 3),
        cloud_sub_(nh_, "/camera/rgb/points", 3),
        sync_(CameraSyncPolicy(10), image_sub_, cloud_sub_),
        msgs_(0),   
        debug_(false)
    {
        ros::NodeHandle nh ("~");
        // load parameters for hough transform
        if (!nh.getParam ("h_rho", h_rho_))
            h_rho_ = 1;
        ROS_INFO ("Hough Rho: %d", h_rho_);  
        if (!nh.getParam ("h_threshold", h_threshold_))
            h_threshold_ = 50;
        ROS_INFO ("Hough Threshold: %d", h_threshold_); 
        if (!nh.getParam ("h_min_length", h_min_length_))
            h_min_length_ = 100;
        ROS_INFO ("Hough Min Length: %d", h_min_length_);    

        // create a window to display results in
        if (debug_) cv::namedWindow("chess_board_locator");
        pub_ = nh.advertise< pcl::PointCloud<point> >("points", 10);

        sync_.registerCallback(boost::bind(&ChessBoardLocator::cameraCallback, this, _1, _2));
    }

    /* 
     * Determine transform for chess board
     */
    void cameraCallback ( const sensor_msgs::ImageConstPtr& image,
                          const sensor_msgs::PointCloud2ConstPtr& depth)
    {
        // convert image
        try
        {
            bridge_ = cv_bridge::toCvCopy(image, "bgr8");
            ROS_INFO("New image/cloud.");
        }
        catch(cv_bridge::Exception& e)
        {
           ROS_ERROR("Conversion failed");
        }
        // convert cloud from sensor message
        pcl::PointCloud<color_point> cloud;
        pcl::fromROSMsg(*depth, cloud);

        // segment based on a channel (blue board squares)
        cv::Mat dst, cdst;
        cv::Mat src(bridge_->image.rows, bridge_->image.cols, CV_8UC1);
        for(int i = 0; i < bridge_->image.rows; i++)
        {
            char* Di = bridge_->image.ptr<char>(i);
            char* Ii = src.ptr<char>(i);
            for(int j = 0; j < bridge_->image.cols; j++)
            {   
                Ii[j] = Di[j*3];
            }   
        }
 
        // threshold, erode/dilate to clean up image
        cv::threshold(src, src, 100, 255, cv::THRESH_BINARY);
        cv::erode(src, src, cv::Mat());
        cv::dilate(src, src, cv::Mat());
        // edge detection, dilation before hough transform 
        cv::Canny(src, dst, 30, 200, 3); 
        cv::dilate(dst, dst, cv::Mat());

        // do a hough transformation to find lines
        vector<cv::Vec4i> lines;
        cv::HoughLinesP(dst, lines, h_rho_, CV_PI/180, h_threshold_, h_min_length_, 10 );
        ROS_DEBUG("Found %d lines", (int) lines.size());

        // split into vertical/horizontal lines
        vector<int> h_indexes, v_indexes;
        for( size_t i = 0; i < lines.size(); i++ )
        {
            cv::Vec4i l = lines[i];
            int dx = l[2]-l[0]; int dy = l[3]-l[1];
            if(abs(dx) > abs(dy)){
                h_indexes.push_back(i);
            }else{
                v_indexes.push_back(i);
            }
        }

        // output lines to screen
        if(debug_)
        {
            // convert back to color
            cv::cvtColor(dst, cdst, CV_GRAY2BGR);
            
            // then red/green for horizontal/vertical
            ROS_DEBUG("horizontal lines: %d", (int) h_indexes.size());
            for( size_t i = 0; i < h_indexes.size(); i++ )
            {
                cv::Vec4i l = lines[h_indexes[i]];
                cv::line( cdst, cv::Point(l[0], l[1]), cv::Point(l[2], l[3]), cv::Scalar(0,0,255), 3, CV_AA);
            }
            ROS_DEBUG("vertical lines: %d", (int) v_indexes.size());
            for( size_t i = 0; i < v_indexes.size(); i++ )
            {
                cv::Vec4i l = lines[v_indexes[i]];
                cv::line( cdst, cv::Point(l[0], l[1]), cv::Point(l[2], l[3]), cv::Scalar(0,255,0), 3, CV_AA);
            }
        }

        // get all intersections
        pcl::PointCloud<point> data;
        data.header.frame_id  = depth->header.frame_id;
        data.header.stamp  = depth->header.stamp;
        for( size_t i = 0; i < h_indexes.size(); i++ )
        {
            cv::Vec4i hl = lines[h_indexes[i]];
            for( size_t j = 0; j < v_indexes.size(); j++ )
            {
                cv::Vec4i vl = lines[v_indexes[j]];
                cv::Point p = findIntersection(hl,vl);
                if(p.x > 0 && p.y > 0){
                    color_point cp = cloud(p.x,p.y);
                    bool include = true;
                    for(size_t k = 0; k < data.points.size(); k++)
                    {   
                        point tp = data.points[k];
                        if( abs(tp.x-cp.x) + abs(tp.y-cp.y) + abs(tp.z-cp.z) < 0.03 ){
                            include = false;
                            break;
                        }
                    }
                    if(include)
                        data.push_back( point(cp.x,cp.y,cp.z) );
                    if(debug_)
                    {
                        cv::circle( cdst, p, 5, cv::Scalar(255,0,0), -1 );
                    }
                }
            }
        }
        ROS_DEBUG("Created data cloud of size %d", (int)data.points.size());

        // find centroid of intersections
        Eigen::Vector4f centroid; 
        pcl::compute3DCentroid(data, centroid);

        // find corner candidates - x right, y down
        vector<int> a1_candidates, a8_candidates, h1_candidates;
        for( size_t i = 0; i < data.points.size(); i++ )
        {
            point p = data.points[i];
            if( (p.x < centroid[0]-0.05) && (p.y > centroid[1]+0.05) )
                a1_candidates.push_back(i);
            else if( (p.x < centroid[0]-0.05) && (p.y < centroid[1]-0.05) )
                a8_candidates.push_back(i);
            else if( (p.x > centroid[0]+0.05) && (p.y > centroid[1]+0.05) )
                h1_candidates.push_back(i);
        }

        // ideal board of a1, a8, h1        
        pcl::PointCloud<point> board;
        board.push_back( point(0.05715, 0.05715, 0) );   // a1
        board.push_back( point(0.05715, 0.05715*7, 0) ); // a8
        board.push_back( point(0.05715*7, 0.05715, 0) ); // h1
        // evaluate candidates
        float best_score = -1.0;
        Eigen::Matrix4f best_transform;
        ROS_DEBUG("Evaluating %d candidates", (int) (a1_candidates.size() * a8_candidates.size() * h1_candidates.size()));
        for( size_t i = 0; i < a1_candidates.size(); i++ ){
            for( size_t j = 0; j < a8_candidates.size(); j++ ){
                for( size_t k = 0; k < h1_candidates.size(); k++ ){
                    Eigen::Matrix4f t;
                    point a1 = data.points[a1_candidates[i]];
                    point a8 = data.points[a8_candidates[j]];
                    point h1 = data.points[h1_candidates[k]];
                    // construct a basis
                    pcl::PointCloud<point> candidates;
                    candidates.push_back(a1);
                    candidates.push_back(a8);
                    candidates.push_back(h1);
                    // estimate transform
                    pcl::estimateRigidTransformationSVD( candidates, board, t );
                    // transform whole cloud
                    pcl::PointCloud<point> data_transformed;
                    pcl::transformPointCloud( data, data_transformed, t );
                    // compute error
                    float error = 0.0;
                    for( size_t p = 0; p < data_transformed.points.size(); p++)
                    {
                        point pt = data_transformed.points[p];
                        // TODO: can we speed this up?
                        float e = 1000;
                        for( int x = 1; x < 8; x++)
                        {   
                            for( int y = 1; y < 8; y++)
                            {
                                float iter = (0.05715*x-pt.x)*(0.05715*x-pt.x)+(0.05715*y-pt.y)*(0.05715*y-pt.y);
                                if(iter < e)
                                    e = iter;
                            }
                        }
                        error += e;
                    }
                    // update
                    if( (best_score < 0) || (error < best_score) )
                    {
                        best_score = error;
                        best_transform = t;
                    }                    
                }
            }
        }    
        ROS_DEBUG("final score %f", best_score);   
          
        // publish transform
        tf::Transform transform = tfFromEigen(best_transform.inverse());
        br_.sendTransform(tf::StampedTransform(transform, ros::Time::now(), depth->header.frame_id, "chess_board"));
        ROS_INFO("published %d", msgs_++);

        if(debug_){
            // publish the cloud
            pcl::PointCloud<point> data_transformed;
            data_transformed.header.frame_id  = depth->header.frame_id;
            data_transformed.header.stamp  = depth->header.stamp;
            pcl::transformPointCloud( data, data_transformed, best_transform);
            pub_.publish(data_transformed);
            // show the image
            cv::imshow("chess_board_locator", cdst);
            cv::imwrite("image.png", cdst);
            cv::waitKey(3);
        }
    }

  private: 
    /* node handles, subscribers, publishers, etc */
    ros::NodeHandle nh_;
    message_filters::Subscriber<sensor_msgs::Image> image_sub_; 
    message_filters::Subscriber<sensor_msgs::PointCloud2> cloud_sub_;
    message_filters::Synchronizer<CameraSyncPolicy> sync_;
    ros::Publisher pub_;
    tf::TransformBroadcaster br_;
    cv_bridge::CvImagePtr bridge_;

    /* parameters for hough line detection */
    int h_rho_;
    int h_threshold_;
    int h_min_length_;
    int msgs_;
    bool debug_;
};

int main (int argc, char **argv)
{
  ros::init(argc, argv, "chess_board_locator");
  ros::NodeHandle n;
  ChessBoardLocator locator(n);
  ros::spin();
  return 0;
}


