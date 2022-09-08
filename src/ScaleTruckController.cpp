#include "scale_truck_control/ScaleTruckController.hpp"

namespace scale_truck_control{

ScaleTruckController::ScaleTruckController(ros::NodeHandle nh)
    : nodeHandle_(nh), laneDetector_(nodeHandle_), ZMQ_SOCKET_(nh), imageTransport_(nh){
  if (!readParameters()) {
    ros::requestShutdown();
  }

  init();
}

ScaleTruckController::~ScaleTruckController() {
  isNodeRunning_ = false;

  scale_truck_control::xav2lrc msg;
  msg.tar_vel = ResultVel_;
  {
    std::scoped_lock lock(dist_mutex_);
    msg.steer_angle = AngleDegree_;
    msg.cur_dist = distance_;
  }
  {
    std::scoped_lock lock(rep_mutex_);
    msg.tar_dist = TargetDist_;
    msg.fi_encoder = fi_encoder_;
    msg.fi_camera = fi_camera_;
    msg.fi_lidar = fi_lidar_;
    msg.beta = beta_;
    msg.gamma = gamma_;
  }

  XavPublisher_.publish(msg);
  controlThread_.join();
  tcpThread_.join();
  if (send_rear_camera_image_ && (index_ == 0 || index_ == 1)) tcpImgReqThread_.join();
  if (req_lv_ && (index_ == 1 || index_ == 2)) tcpImgRepThread_.join();

  ROS_INFO("[ScaleTruckController] Stop.");
}

bool ScaleTruckController::readParameters() {
  /***************/
  /* View Option */
  /***************/
  nodeHandle_.param("image_view/enable_opencv", viewImage_, true);
  nodeHandle_.param("image_view/wait_key_delay", waitKeyDelay_, 3);
  nodeHandle_.param("image_view/enable_console_output", enableConsoleOutput_, true);

  /***********************************/
  /* Rear camera sensor usage Option */
  /***********************************/
  nodeHandle_.param("rear_camera_usage/use", rear_camera_, false);

  /*******************/
  /* Velocity Option */
  /*******************/
  nodeHandle_.param("params/index", index_, 0);
  nodeHandle_.param("params/target_vel", TargetVel_, 0.5f); // m/s
  nodeHandle_.param("params/safety_vel", SafetyVel_, 0.3f); // m/s
  nodeHandle_.param("params/fv_max_vel", FVmaxVel_, 0.8f); // m/s
  nodeHandle_.param("params/ref_vel", RefVel_, 0.0f); // m/s
  nodeHandle_.param("params/rcm_vel", RCMVel_, 0.8f);
  
  /*******************/
  /* Distance Option */
  /*******************/
  nodeHandle_.param("params/lv_stop_dist", LVstopDist_, 0.5f); // m
  nodeHandle_.param("params/fv_stop_dist", FVstopDist_, 0.5f); // m
  nodeHandle_.param("params/safety_dist", SafetyDist_, 1.5f); // m
  nodeHandle_.param("params/target_dist", TargetDist_, 0.8f); // m
  nodeHandle_.param("params/rcm_dist", RCMDist_, 0.8f);

  return true;
}

void ScaleTruckController::init() {
  ROS_INFO("[ScaleTruckController] init()");  
  
  gettimeofday(&laneDetector_.start_, NULL);
  
  std::string imageTopicName;
  int imageQueueSize;
  std::string rearImageTopicName;
  int rearImageQueueSize;
  std::string objectTopicName;
  int objectQueueSize; 
  std::string XavSubTopicName;
  int XavSubQueueSize;
  std::string bboxTopicName;
  int bboxQueueSize;

  std::string XavPubTopicName;
  int XavPubQueueSize;
  std::string runYoloTopicName;
  int runYoloQueueSize;

  /******************************/
  /* Ros Topic Subscribe Option */
  /******************************/
  nodeHandle_.param("subscribers/camera_reading/topic", imageTopicName, std::string("/usb_cam/image_raw"));
  nodeHandle_.param("subscribers/camera_reading/queue_size", imageQueueSize, 1);
  nodeHandle_.param("subscribers/rear_camera_reading/topic", rearImageTopicName, std::string("/rear_cam/image_raw"));
  nodeHandle_.param("subscribers/rear_camera_reading/queue_size", rearImageQueueSize, 1);
  nodeHandle_.param("subscribers/obstacle_reading/topic", objectTopicName, std::string("/raw_obstacles"));
  nodeHandle_.param("subscribers/obstacle_reading/queue_size", objectQueueSize, 100);
  nodeHandle_.param("subscribers/lrc_to_xavier/topic", XavSubTopicName, std::string("/lrc2xav_msg"));
  nodeHandle_.param("subscribers/lrc_to_xavier/queue_size", XavSubQueueSize, 1);
  nodeHandle_.param("subscribers/yolo_detector/topic", bboxTopicName, std::string("/yolo_object_detection/bounding_box"));
  nodeHandle_.param("subscribers/yolo_detector/queue_size", bboxQueueSize, 1);
  
  /****************************/
  /* Ros Topic Publish Option */
  /****************************/
  nodeHandle_.param("publishers/xavier_to_lrc/topic", XavPubTopicName, std::string("/xav2lrc_msg"));
  nodeHandle_.param("publishers/xavier_to_lrc/queue_size", XavPubQueueSize, 1);
  nodeHandle_.param("publishers/run_yolo/topic", runYoloTopicName, std::string("/run_yolo_flag"));
  nodeHandle_.param("publishers/run_yolo/queue_size", runYoloQueueSize, 1);

  /************************/
  /* Ros Topic Subscriber */
  /************************/
  imageSubscriber_ = nodeHandle_.subscribe(imageTopicName, imageQueueSize, &ScaleTruckController::imageCallback, this);
  if (rear_camera_) rearImageSubscriber_ = nodeHandle_.subscribe(rearImageTopicName, rearImageQueueSize, &ScaleTruckController::rearImageCallback, this);
  objectSubscriber_ = nodeHandle_.subscribe(objectTopicName, objectQueueSize, &ScaleTruckController::objectCallback, this);
  XavSubscriber_ = nodeHandle_.subscribe(XavSubTopicName, XavSubQueueSize, &ScaleTruckController::XavSubCallback, this);
  ScanSubError = nodeHandle_.subscribe("/scan_error", 1000, &ScaleTruckController::ScanErrorCallback, this);  
  bboxSubscriber_ = nodeHandle_.subscribe(bboxTopicName, bboxQueueSize, &ScaleTruckController::bboxCallback, this);

  /***********************/
  /* Ros Topic Publisher */
  /***********************/
  imgPublisher_ = imageTransport_.advertise("/preceding_truck_image", 1);
  runYoloPublisher_ = nodeHandle_.advertise<scale_truck_control::yolo_flag>(runYoloTopicName, runYoloQueueSize);
  XavPublisher_ = nodeHandle_.advertise<scale_truck_control::xav2lrc>(XavPubTopicName, XavPubQueueSize);

  /**********************/
  /* Safety Start Setup */
  /**********************/
  distance_ = 10.f;
  distAngle_ = 0;

  /************/
  /* ZMQ Data */
  /************/
  zmq_data_ = new ZmqData;
  zmq_data_->src_index = index_;
  zmq_data_->tar_index = 20;  //Control center

  img_data_ = new ImgData;
  img_data_->src_index = index_;
  img_data_->tar_index = index_+1;

  x_ = 0;
  y_ = 0;
  w_ = 0;
  h_ = 0;

  /**********************************/
  /* Control & Communication Thread */
  /**********************************/
  controlThread_ = std::thread(&ScaleTruckController::spin, this);
  tcpThread_ = std::thread(&ScaleTruckController::reply, this, zmq_data_);
//  if (index_ == 0){
//    tcpImgThread_ = std::thread(&ScaleTruckController::requestImage, this, img_data_);
//  }
//  else if (index_ == 1 || index_ == 2){
//    tcpImgThread_ = std::thread(&ScaleTruckController::replyImage, this);
//  }
}

bool ScaleTruckController::getImageStatus(void){
  std::scoped_lock lock(image_mutex_);
  return imageStatus_;
}

void* ScaleTruckController::lanedetectInThread() {
  static int cnt = 10;
  Mat dst;
  std::vector<Mat>channels;
  int count = 0;
  float AngleDegree;
  {
    std::scoped_lock lock(rep_mutex_, image_mutex_);
    if((!camImageTmp_.empty()) && (cnt != 0) && (TargetVel_ > 0.001f))
    {
      bitwise_xor(camImageCopy_,camImageTmp_, dst);
      split(dst, channels);
      for(int ch = 0; ch<dst.channels();ch++) {
        count += countNonZero(channels[ch]);
      }
      {
        if(count == 0 && fi_camera_)
          cnt -= 1;
        else 
          cnt = 10;
      }
    }
    camImageTmp_ = camImageCopy_.clone();
  }
  {
    std::scoped_lock lock(lane_mutex_, vel_mutex_);
    laneDetector_.get_steer_coef(CurVel_);
  }
  {
    std::scoped_lock lock(lane_mutex_, bbox_mutex_);
    laneDetector_.name_ = name_;
    laneDetector_.x_ = x_;
    laneDetector_.y_ = y_;
    laneDetector_.h_ = h_;
    laneDetector_.w_ = w_;
  }
  {
    std::unique_lock<std::mutex> lock(lane_mutex_);
    std::scoped_lock(bbox_mutex_);
    cv_.wait(lock, [this] {return droi_ready_; });

    AngleDegree = laneDetector_.display_img(camImageTmp_, waitKeyDelay_, viewImage_);
    droi_ready_ = false;
  }
  if(cnt == 0){
    {
      std::scoped_lock lock(rep_mutex_);
      beta_ = true;
    }
    {
      std::scoped_lock lock(dist_mutex_);
      AngleDegree_ = -distAngle_;
    }
  }
  else{
    std::scoped_lock lock(dist_mutex_);
    AngleDegree_ = AngleDegree;
  }
}

void* ScaleTruckController::objectdetectInThread() {
  float rotation_angle = 0.0f;
  float lateral_offset = 0.0f;
  float Lw = 0.40f; // 0.236 0.288 0.340 
  float dist, Ld, angle, angle_A;
  float dist_tmp, angle_tmp;
  dist_tmp = 10.f; 
  /**************/
  /* Lidar Data */
  /**************/
  {
    std::scoped_lock lock(object_mutex_, rep_mutex_);
    ObjSegments_ = Obstacle_.segments.size();
    ObjCircles_ = Obstacle_.circles.size();
  
    for(int i = 0; i < ObjCircles_; i++)
    {
      //dist = sqrt(pow(Obstacle_.circles[i].center.x,2)+pow(Obstacle_.circles[i].center.y,2));
      Obstacle_.circles[i].center.y += 0.03f;
      dist = -Obstacle_.circles[i].center.x - Obstacle_.circles[i].true_radius;
      angle = atanf(Obstacle_.circles[i].center.y/Obstacle_.circles[i].center.x)*(180.0f/M_PI);
      if(dist_tmp >= dist) {
        dist_tmp = dist;
        angle_tmp = angle;
        Ld = sqrt(pow(Obstacle_.circles[i].center.x-Lw, 2) + pow(Obstacle_.circles[i].center.y, 2));
        angle_A = atanf(Obstacle_.circles[i].center.y/(Obstacle_.circles[i].center.x-Lw));
        ampersand_ = atanf(2*Lw*sin(angle_A)/Ld) * (180.0f/M_PI); // pure pursuit
      }
    }
    
    {
      std::scoped_lock lock(lane_mutex_);
      log_est_dist_ = laneDetector_.est_dist_;
      if(gamma_ == true && laneDetector_.est_dist_ != 0){
        dist_tmp = laneDetector_.est_dist_;
      }
    }
    if(beta_ == true){
      angle_tmp = ampersand_;
    }
  }
  if(ObjCircles_ != 0)
  {
    std::scoped_lock lock(dist_mutex_);
    distance_ = dist_tmp;
    distAngle_ = angle_tmp;
  }

  /*****************************/
  /* Dynamic ROI Distance Data */
  /*****************************/
  {
    std::scoped_lock lock(lane_mutex_);
    if(dist_tmp < 1.24 && dist_tmp > 0.30) // 1.26 ~ 0.28
    {
      laneDetector_.distance_ = (int)((1.24 - dist_tmp)*490.0);
    }
//    if(dist_tmp < 1.2 && dist_tmp > 0.24) // 1.26 ~ 0.28
//    {
//      laneDetector_.distance_ = (int)((1.2 - dist_tmp)*500.0);
//    }
    else {
      laneDetector_.distance_ = 0;
    }
    droi_ready_ = true;
    cv_.notify_one();
  }  
  if(index_ == 0){  //LV
    std::scoped_lock lock(rep_mutex_, dist_mutex_);
    if(distance_ <= LVstopDist_) {
    // Emergency Brake
      ResultVel_ = 0.0f;
    }
    else if (distance_ <= SafetyDist_){
      float TmpVel_ = (ResultVel_-SafetyVel_)*((distance_-LVstopDist_)/(SafetyDist_-LVstopDist_))+SafetyVel_;
      if (TargetVel_ < TmpVel_){
        ResultVel_ = TargetVel_;
      }
      else{
        ResultVel_ = TmpVel_;
      }
    }
    else{
      ResultVel_ = TargetVel_;
    }
  }
  else{  //FVs
    std::scoped_lock lock(rep_mutex_, dist_mutex_);
    if ((distance_ <= FVstopDist_) || (TargetVel_ <= 0.1f)){
    // Emergency Brake
      ResultVel_ = 0.0f;
    }
    else {
      ResultVel_ = TargetVel_;
    }
  }
}

void ScaleTruckController::imageCompress(cv::Mat camImage) {
  std::vector<int> comp;
  comp.push_back(1); // CV_IMWRITE_JPEG_QUALITY
  comp.push_back(80); // 0 ~ 100
  cv::imencode(".jpg", camImage, compImageSend_, comp);
}

void ScaleTruckController::requestImage(ImgData* img_data)
{
  while(isNodeRunning_){
    {
      std::scoped_lock lock(rear_image_mutex_);
      rearImageTmp_ = rearImageCopy_;
    }
    if(!rearImageTmp_.empty()){
      imageCompress(rearImageTmp_);
      if(compImageSend_.size() <= (sizeof(img_data->comp_image) / sizeof(u_char))){
        std::copy(compImageSend_.begin(), compImageSend_.end(), img_data->comp_image);
      }
      else printf("Warning !! compressed image size is bigger than comp_img array size in ImgData\n");
      img_data->size = compImageSend_.size();
      req_check_++;
      gettimeofday(&img_data->startTime, NULL);
      ZMQ_SOCKET_.requestImageZMQ(img_data); 
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } 
}

void ScaleTruckController::replyImage()
{
  while(isNodeRunning_){
    struct timeval endTime;
    ZMQ_SOCKET_.replyImageZMQ();
    img_data_ = ZMQ_SOCKET_.img_recv_;
    compImageRecv_.resize(img_data_->size);
    memcpy(&compImageRecv_[0], &img_data_->comp_image[0], img_data_->size);
    rearImageJPEG_ = imdecode(Mat(compImageRecv_), IMREAD_COLOR);
    gettimeofday(&endTime, NULL);

    //image publish
    sensor_msgs::ImagePtr msg = cv_bridge::CvImage(std_msgs::Header(), "bgr8", rearImageJPEG_).toImageMsg();
    msg->header.stamp.sec = img_data_->startTime.tv_sec;
    msg->header.stamp.nsec = img_data_->startTime.tv_usec;
    imgPublisher_.publish(msg);

    rep_check_++;
    if (rep_check_ > 0) time_ += ((endTime.tv_sec - img_data_->startTime.tv_sec) * 1000.0) + ((endTime.tv_usec - img_data_->startTime.tv_usec)/1000.0);
    else time_ = 0.0;

    DelayTime_ = time_ / (double)rep_check_;

    if (rep_check_ > 3000){
      time_ = 0.0;
      rep_check_ = 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void ScaleTruckController::reply(ZmqData* zmq_data){
  while(isNodeRunning_){
    static float t_vel = 0.0f;
    static float t_dist = 0.8f;
    if(zmq_data->tar_index == 20){
      {
        std::scoped_lock lock(vel_mutex_, dist_mutex_);
	zmq_data->cur_vel = CurVel_;
	zmq_data->cur_dist = distance_;
	zmq_data->cur_angle = AngleDegree_;
      }
      {
        std::scoped_lock lock(lane_mutex_);
        zmq_data->coef[0].a = laneDetector_.lane_coef_.left.a;
        zmq_data->coef[0].b = laneDetector_.lane_coef_.left.b;
        zmq_data->coef[0].c = laneDetector_.lane_coef_.left.c;
        zmq_data->coef[1].a = laneDetector_.lane_coef_.right.a;
        zmq_data->coef[1].b = laneDetector_.lane_coef_.right.b;
        zmq_data->coef[1].c = laneDetector_.lane_coef_.right.c;
        zmq_data->coef[2].a = laneDetector_.lane_coef_.center.a;
        zmq_data->coef[2].b = laneDetector_.lane_coef_.center.b;
        zmq_data->coef[2].c = laneDetector_.lane_coef_.center.c;
      }
      ZMQ_SOCKET_.replyZMQ(zmq_data);
    }
    {
      std::scoped_lock lock(rep_mutex_, mode_mutex_);
      if(index_ == 0){
        if(crc_mode_ == 2){
          TargetVel_ = 0;
	}
	else if(crc_mode_ == 1){
          if(t_vel > RCMVel_) TargetVel_ = RCMVel_;
          else TargetVel_ = t_vel;
	  if(t_dist < RCMDist_) TargetDist_ = RCMDist_;
	  else TargetDist_ = t_dist;
	}
      }
      if(ZMQ_SOCKET_.rep_recv_->src_index == 20){
        if(index_ == 0){  //LV 
          t_vel = ZMQ_SOCKET_.rep_recv_->tar_vel;
          t_dist = ZMQ_SOCKET_.rep_recv_->tar_dist;
	  if(crc_mode_ == 1){  //RCM
            if (t_vel > RCMVel_) TargetVel_ = RCMVel_;
	    else TargetVel_ = t_vel;
	    if (t_dist < RCMDist_) TargetDist_ = RCMDist_;
	    else TargetDist_ = t_dist;
	  }
	  else{  //TM
            TargetVel_ = t_vel;
            TargetDist_ = t_dist;
	  }
	}
	fi_encoder_ = ZMQ_SOCKET_.rep_recv_->fi_encoder;
        fi_camera_ = ZMQ_SOCKET_.rep_recv_->fi_camera;
        fi_lidar_ = ZMQ_SOCKET_.rep_recv_->fi_lidar;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

void ScaleTruckController::displayConsole() {
  static std::string ipAddr = ZMQ_SOCKET_.getIPAddress();

  printf("\033[2J");
  printf("\033[1;1H");
  printf("%s (%s) - %s\n","-Client", ipAddr.c_str() , ZMQ_SOCKET_.udp_ip_.c_str());
  printf("\nAngle\t\t\t: %2.3f degree", AngleDegree_);
  printf("\nRefer Vel\t\t: %3.3f m/s", RefVel_);
  printf("\nSend Vel\t\t: %3.3f m/s", ResultVel_);
  printf("\nTar/Cur Vel\t\t: %3.3f / %3.3f m/s", TargetVel_, CurVel_);
  printf("\nTar/Cur Dist\t\t: %3.3f / %3.3f m", TargetDist_, distance_);
  printf("\nEncoder, Camera, Lidar Failure: %d / %d / %d", fi_encoder_, fi_camera_, fi_lidar_);
  printf("\nAlpha, Beta, Gamma\t: %d / %d / %d", alpha_, beta_, gamma_);
  printf("\nCRC mode, LRC mode\t: %d / %d", crc_mode_, lrc_mode_);
  printf("\nK1/K2\t\t\t: %3.3f / %3.3f", laneDetector_.K1_, laneDetector_.K2_);
  printf("\nLdrErrMsg\t\t\t: %x", LdrErrMsg_);
  printf("\nx / y / w / h\t\t: %u / %u / %u / %u", x_, y_, w_, h_);
  printf("\nREQ Check\t\t: %d", req_check_);
  printf("\nREP Check\t\t: %d", rep_check_);
  if(!compImageSend_.empty()){
    printf("\nSending image size\t: %zu", compImageSend_.size());
  }
  printf("\nCycle Time\t\t: %3.3f ms", CycleTime_);
  if(ObjCircles_ > 0) {
    printf("\nCirs\t\t\t: %d", ObjCircles_);
    printf("\nDistAng\t\t\t: %2.3f degree", distAngle_);
  }
  printf("\n");
}

void ScaleTruckController::recordData(struct timeval startTime){
  struct timeval currentTime;
  char file_name[] = "SCT_log00.csv";
  static char file[128] = {0x00, };
  char buf[256] = {0x00,};
  static bool flag = false;
  double diff_time;
  ifstream read_file;
  ofstream write_file;
  string log_path = "/home/jetson/catkin_ws/logfiles/";
  if(!flag){
    for(int i = 0; i < 100; i++){
      file_name[7] = i/10 + '0';  //ASCII
      file_name[8] = i%10 + '0';
      sprintf(file, "%s%s", log_path.c_str(), file_name);
      read_file.open(file);
      if(read_file.fail()){  //Check if the file exists
        read_file.close();
        write_file.open(file);
        break;
      }
      read_file.close();
    }
    write_file << "time,measured_dist,estimated_dist" << endl; //seconds
    flag = true;
  }
  if(flag){
    std::scoped_lock lock(dist_mutex_);
    gettimeofday(&currentTime, NULL);
    diff_time = ((currentTime.tv_sec - startTime.tv_sec)) + ((currentTime.tv_usec - startTime.tv_usec)/1000000.0);
    sprintf(buf, "%.10e, %.3f, %.3f", diff_time, distance_, log_est_dist_);
    write_file.open(file, std::ios::out | std::ios::app);
    write_file << buf << endl;
  }
  write_file.close();
}

void ScaleTruckController::spin() {
  double diff_time=0.0;
  int cnt = 0;
  
  const auto wait_duration = std::chrono::milliseconds(2000);
  while(!getImageStatus()) {
    printf("Waiting for image.\n");
    if(!isNodeRunning_) {
      return;
    }
    std::this_thread::sleep_for(wait_duration);
  }
  
  scale_truck_control::xav2lrc msg;
  scale_truck_control::yolo_flag yolo_flag_msg;
  std::thread lanedetect_thread;
  std::thread objectdetect_thread;
  
  const auto wait_image = std::chrono::milliseconds(20);

  while(!controlDone_ && ros::ok()) {
    struct timeval start_time, end_time;
    gettimeofday(&start_time, NULL);
    lanedetect_thread = std::thread(&ScaleTruckController::lanedetectInThread, this);
    objectdetect_thread = std::thread(&ScaleTruckController::objectdetectInThread, this);
    
    lanedetect_thread.join();
    objectdetect_thread.join();    

    if(enableConsoleOutput_)
      displayConsole();

    if (gamma_ && !run_yolo_){
      run_yolo_ = true;
      yolo_flag_msg.run_yolo = true;
      runYoloPublisher_.publish(yolo_flag_msg);
    }

    if (gamma_ && beta_ && !req_lv_){
      req_lv_ = true;
    }

    msg.tar_vel = ResultVel_;  //Xavier to LRC and LRC to OpenCR
    {
      std::scoped_lock lock(dist_mutex_);
      msg.steer_angle = AngleDegree_;
      msg.cur_dist = distance_;
    }
    {
      std::scoped_lock lock(rep_mutex_);
      msg.tar_dist = TargetDist_;
      msg.fi_encoder = fi_encoder_;
      msg.fi_camera = fi_camera_;
      msg.fi_lidar = fi_lidar_;
      msg.beta = beta_;
      msg.gamma = gamma_;
    }
    XavPublisher_.publish(msg);

    if(!isNodeRunning_) {
      controlDone_ = true;
      ZMQ_SOCKET_.controlDone_ = true;
      ros::requestShutdown();
    }

    gettimeofday(&end_time, NULL);
    diff_time += ((end_time.tv_sec - start_time.tv_sec) * 1000.0) + ((end_time.tv_usec - start_time.tv_usec) / 1000.0);
    cnt++;

    CycleTime_ = diff_time / (double)cnt;

//    printf("cnt: %d\n", cnt);
    if (cnt > 3000){
      diff_time = 0.0;
      cnt = 0;
    }

    recordData(laneDetector_.start_);

    if (!tcp_img_req_ && send_rear_camera_image_ && (index_ == 0 || index_ == 1)){
      tcpImgReqThread_ = std::thread(&ScaleTruckController::requestImage, this, img_data_);
      tcp_img_req_ = true;
    }

    if (!tcp_img_rep_ && req_lv_ && (index_ == 1 || index_ == 2)){
      tcpImgRepThread_ = std::thread(&ScaleTruckController::replyImage, this);
      tcp_img_rep_ = true;
    }
  }
}

void ScaleTruckController::ScanErrorCallback(const std_msgs::UInt32::ConstPtr &msg) {
   LdrErrMsg_ = msg->data;
   if(fi_lidar_) {
     LdrErrMsg_ = 0x80008002;
   }
   {
     std::scoped_lock lock(rep_mutex_);
     if(LdrErrMsg_){
       gamma_ = true;
     }
   }
}

void ScaleTruckController::objectCallback(const obstacle_detector::Obstacles &msg) {
  {
    std::scoped_lock lock(object_mutex_);
    Obstacle_ = msg;
  }
}

void ScaleTruckController::imageCallback(const sensor_msgs::ImageConstPtr &msg) {
  cv_bridge::CvImagePtr cam_image;
  try{
    cam_image = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);
  } catch (cv_bridge::Exception& e) {
    ROS_ERROR("cv_bridge exception : %s", e.what());
  }

  {
    std::scoped_lock lock(rep_mutex_, image_mutex_);
    if(cam_image && !fi_camera_) {
      imageHeader_ = msg->header;
      camImageCopy_ = cam_image->image.clone();
      imageStatus_ = true;
    }
  }
}

void ScaleTruckController::rearImageCallback(const sensor_msgs::ImageConstPtr &msg) {
  cv_bridge::CvImagePtr cam_image;
  try{
    cam_image = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);
  } catch (cv_bridge::Exception& e) {
    ROS_ERROR("cv_bridge exception : %s", e.what());
  }

  {
    std::scoped_lock lock(rear_image_mutex_);
    if(cam_image) {
      rearImageCopy_ = cam_image->image.clone();
    }
  }
}

void ScaleTruckController::XavSubCallback(const scale_truck_control::lrc2xav &msg){
  {
    std::scoped_lock lock(mode_mutex_);
    alpha_ = msg.alpha;
    lrc_mode_ = msg.lrc_mode;
    crc_mode_ = msg.crc_mode;
  }
  {
    std::scoped_lock lock(vel_mutex_);
    CurVel_ = msg.cur_vel;
  }
  if (index_ != 0){  //FVs
    std::scoped_lock lock(rep_mutex_);
    TargetVel_ = msg.tar_vel;
    TargetDist_ = msg.tar_dist;
  }
  send_rear_camera_image_ = msg.send_rear_camera_image;
}

void ScaleTruckController::bboxCallback(const yolo_object_detection::bounding_box &msg){
  {
    std::scoped_lock lock(bbox_mutex_);
    name_ = msg.name;
    if ((msg.x > 0 && msg.x < 640) && \
        (msg.y > 0 && msg.y < 480) && \
	(msg.w > 0 && msg.w < 640) && \
	(msg.h > 0 && msg.h < 480)){
      x_ = msg.x;
      y_ = msg.y;
      w_ = msg.w;
      h_ = msg.h;
    }
  }
}

} /* namespace scale_truck_control */ 
