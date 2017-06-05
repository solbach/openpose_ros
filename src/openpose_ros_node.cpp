#define USE_CAFFE
#include <ros/ros.h>
#include <image_transport/image_transport.h>
#include <cv_bridge/cv_bridge.h>
#include <sensor_msgs/image_encodings.h>

#include <chrono> // `std::chrono::` functions and classes, e.g. std::chrono::milliseconds
#include <cstdio> // sscanf
#include <string> // std::string

#include <opencv2/core/core.hpp> // cv::Mat & cv::Size

#include <gflags/gflags.h> // DEFINE_bool, DEFINE_int32, DEFINE_int64, DEFINE_uint64, DEFINE_double, DEFINE_string
#include <glog/logging.h> // google::InitGoogleLogging, CHECK, CHECK_EQ, LOG, VLOG, ...

//#include <openpose/headers.hpp>
// OpenPose dependencies

#include <openpose/core/headers.hpp>
#include <openpose/filestream/headers.hpp>
#include <openpose/gui/headers.hpp>
#include <openpose/utilities/headers.hpp>
#include <openpose/pose/poseExtractorCaffe.hpp>
#include <openpose/pose/poseExtractor.hpp>
#include <openpose/pose/poseParameters.hpp>
#include <openpose/pose/headers.hpp>

static const std::string OPENCV_WINDOW = "Openpose Window";
static const std::string image_topic = "/zed/rgb/image_raw_color";

std::shared_ptr<op::PoseExtractor> g_pose_extractor;

// Gflags in the command line terminal. Check all the options by adding the flag `--help`, e.g. `openpose.bin --help`.
// Note: This command will show you flags for several files. Check only the flags for the file you are checking. E.g. for `openpose.bin`, look for `Flags from examples/openpose/openpose.cpp:`.
// Debugging
DEFINE_int32(logging_level,             3,              "The logging level. Integer in the range [0, 255]. 0 will output any log() message, while 255 will not output any."
        " Current OpenPose library messages are in the range 0-4: 1 for low priority messages and 4 for important ones.");
// OpenPose
DEFINE_string(model_pose,               "COCO",         "Model to be used (e.g. COCO, MPI, MPI_4_layers).");
DEFINE_string(model_folder,             "/home/markus/git/jtl/Fallen Person/CMU-OpenPose/openpose/models/",      "Folder where the pose models (COCO and MPI) are located.");
DEFINE_string(net_resolution,           "656x368",      "Multiples of 16.");
DEFINE_string(resolution,               "-1x-1",     "The image resolution (display). Use \"-1x-1\" to force the program to use the default images resolution.");
DEFINE_int32(num_gpu_start,             0,              "GPU device start number.");
DEFINE_double(scale_gap,                0.3,            "Scale gap between scales. No effect unless num_scales>1. Initial scale is always 1. If you want to change the initial scale, "
        "you actually want to multiply the `net_resolution` by your desired initial scale.");
DEFINE_int32(num_scales,                1,              "Number of scales to average.");
// OpenPose Rendering
DEFINE_double(alpha_pose,               0.6,            "Blending factor (range 0-1) for the body part rendering. 1 will show it completely, 0 will hide it.");


class OpenPoseNode
{
private:
    ros::NodeHandle nh_;
    image_transport::ImageTransport it_;
    image_transport::Subscriber image_sub_;
    cv_bridge::CvImagePtr cv_ptr;
    cv::Mat inputImageMat;

public:
    OpenPoseNode(): it_(nh_)
    {
        // Subscribe to input video feed and publish output video feed
        image_sub_ = it_.subscribe(image_topic, 10, &OpenPoseNode::convert_image, this);
        cv_ptr = nullptr;
    }

    ~OpenPoseNode(){}

    void convert_image(const sensor_msgs::ImageConstPtr& msg)
    {
        try
        {
            cv_ptr = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);
            inputImageMat = cv_ptr->image;
        }
        catch (cv_bridge::Exception& e)
        {
            ROS_ERROR("cv_bridge exception: %s", e.what());
            return;
        }
    }

    cv::Mat get_imageMat()
    {
        return inputImageMat;
    }

    cv_bridge::CvImagePtr& get_cvimage_ptr()
    {
        return cv_ptr;
    }

};


op::PoseModel gflagToPoseModel(const std::string& poseModeString)
{
    op::log("", op::Priority::Low, __LINE__, __FUNCTION__, __FILE__);
    if (poseModeString == "COCO")
        return op::PoseModel::COCO_18;
    else if (poseModeString == "MPI")
        return op::PoseModel::MPI_15;
    else if (poseModeString == "MPI_4_layers")
        return op::PoseModel::MPI_15_4;
    else
    {
        op::error("String does not correspond to any model (COCO, MPI, MPI_4_layers)", __LINE__, __FUNCTION__, __FILE__);
        return op::PoseModel::COCO_18;
    }
}

// Google flags into program variables
std::tuple<cv::Size, cv::Size, cv::Size, op::PoseModel> gflagsToOpParameters()
{
    op::log("", op::Priority::Low, __LINE__, __FUNCTION__, __FILE__);
    // outputSize
    cv::Size outputSize;
    auto nRead = sscanf(FLAGS_resolution.c_str(), "%dx%d", &outputSize.width, &outputSize.height);
    op::checkE(nRead, 2, "Error, resolution format (" +  FLAGS_resolution + ") invalid, should be e.g., 960x540 ", __LINE__, __FUNCTION__, __FILE__);
    // netInputSize
    cv::Size netInputSize;
    nRead = sscanf(FLAGS_net_resolution.c_str(), "%dx%d", &netInputSize.width, &netInputSize.height);
    op::checkE(nRead, 2, "Error, net resolution format (" +  FLAGS_net_resolution + ") invalid, should be e.g., 656x368 (multiples of 16)", __LINE__, __FUNCTION__, __FILE__);
    // netOutputSize
    const auto netOutputSize = netInputSize;
    // poseModel
    const auto poseModel = gflagToPoseModel(FLAGS_model_pose);
    // Check no contradictory flags enabled
    if (FLAGS_alpha_pose < 0. || FLAGS_alpha_pose > 1.)
        op::error("Alpha value for blending must be in the range [0,1].", __LINE__, __FUNCTION__, __FILE__);
    if (FLAGS_scale_gap <= 0. && FLAGS_num_scales > 1)
        op::error("Uncompatible flag configuration: scale_gap must be greater than 0 or num_scales = 1.", __LINE__, __FUNCTION__, __FILE__);
    // Logging and return result
    op::log("", op::Priority::Low, __LINE__, __FUNCTION__, __FILE__);
    return std::make_tuple(outputSize, netInputSize, netOutputSize, poseModel);
}

int opRealTimeProcessing()
{
    op::check(0 <= FLAGS_logging_level && FLAGS_logging_level <= 255, "Wrong logging_level value.", __LINE__, __FUNCTION__, __FILE__);
    op::ConfigureLog::setPriorityThreshold((op::Priority)FLAGS_logging_level);

    // Step 1 - Read Google flags (user defined configuration)
    cv::Size outputSize;
    cv::Size netInputSize;
    cv::Size netOutputSize;
    op::PoseModel poseModel;
    std::tie(outputSize, netInputSize, netOutputSize, poseModel) = gflagsToOpParameters();

    // Step 2 - Initialize all required classes
    op::CvMatToOpInput cvMatToOpInput{netInputSize, FLAGS_num_scales, (float)FLAGS_scale_gap};
    op::CvMatToOpOutput cvMatToOpOutput{outputSize};
    op::PoseExtractorCaffe poseExtractorCaffe{netInputSize, netOutputSize, outputSize, FLAGS_num_scales, (float)FLAGS_scale_gap, poseModel,
                                              FLAGS_model_folder, FLAGS_num_gpu_start};

    op::PoseRenderer poseRenderer{netOutputSize, outputSize, poseModel, nullptr, (float)FLAGS_alpha_pose};
    op::OpOutputToCvMat opOutputToCvMat{outputSize};

    // Step 3 - Initialize resources on desired thread (in this case single thread, i.e. we init resources here)
    poseExtractorCaffe.initializationOnThread();
    poseRenderer.initializationOnThread();

    // Step 4 - Initialize the image subscriber
    OpenPoseNode opn;

    int count = 0;
    const auto timerBegin = std::chrono::high_resolution_clock::now();

    ros::spinOnce();

    // Step 5 - Continuously process images from image subscriber
    while (ros::ok())
    {
        // Step 6 - Get cv_image ptr and check that it is not null
        //cv_bridge::CvImagePtr tmp_image_ptr = opn.get_cvimage_ptr();
        //cv::Mat tmp_image = opn.get_imageMat();
        cv::Mat tmp_image = cv::imread("/home/markus/Downloads/test.jpg", CV_LOAD_IMAGE_COLOR);

        cv::imshow("THIS IS THE INPUT", tmp_image);
        cv::waitKey(5);
        if(!tmp_image.empty())
        //if(false)
        {
            // Step 7 - Format Input and Output Image
            //cv::Mat inputImage = tmp_image_ptr->image;
            ROS_WARN("Entering cvMatToOpInput");
            const auto netInputArray = cvMatToOpInput.format(tmp_image);
            ROS_WARN("Exiting cvMatToOpInput");
            double scaleInputToOutput;
            op::Array<float> outputArray;
            std::tie(scaleInputToOutput, outputArray) = cvMatToOpOutput.format(tmp_image);

            // Step 8 - Estimate poseKeyPoints
            poseExtractorCaffe.forwardPass(netInputArray, tmp_image.size());

            // poseKeyPoints stores the useful information about the humans
            // poseKeyPoints.getSize(0); is the number of people in the frame
            // poseKeyPoints.getSize(1); is the number of body parts tracked (which is 18 for coco)
            // (More information about the order of the body parts is located in the output format on
            //  https://github.com/CMU-Perceptual-Computing-Lab/openpose)
            // Each body part has an x, y, and confidence, so you can access the first body part's x with
            // poseKeyPoints[0], the first body part's y with poseKeyPoints[1], and the first body part's
            // confidence with poseKeyPoints[2]
            const auto poseKeyPoints = poseExtractorCaffe.getPoseKeyPoints();

            // Step 9 - Render the pose 
            poseRenderer.renderPose(outputArray, poseKeyPoints);
            // Step 10 - OpenPose output format to cv::Mat
            auto outputImage = opOutputToCvMat.formatToCvMat(outputArray);

            // Stepp 11 - Show the image
            cv::imshow(OPENCV_WINDOW, outputImage);
            cv::waitKey(1); // It displays the image and sleeps at least 1 ms (it usually sleeps ~5-10 msec to display the image)
            count++;
        }
        ros::spinOnce();
    }

    // Measuring total time
    const auto totalTimeSec = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now()-timerBegin).count() * 1e-9;
    const auto message = "Real-time pose estimation demo successfully finished. Total time: " + std::to_string(totalTimeSec) + " seconds. "
                         + std::to_string(count) + " frames processed. Average FPS is " + std::to_string(count/totalTimeSec);
    op::log(message, op::Priority::Max);

    return 0;
}

int main(int argc, char** argv)
{
    google::InitGoogleLogging("openpose_ros_node");
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    ros::init(argc, argv, "openpose_ros_node");
    ROS_INFO("Node started: openpose_ros_node");

    return opRealTimeProcessing();
}