#include <ros/ros.h>
#include <visualization_msgs/Marker.h>
#include <visualization_msgs/MarkerArray.h>
#include "detection/lidar_tracker.h"
#include "detection/sensor_fusion.h"

extern ros::Publisher lidar_kf_pub, lidar_pub;
extern std::string FIXED_FRAME;
extern int LIDAR_MIN_CONFIDENCE;
extern int LIDAR_MAX_CONFIDENCE;
extern float LIDAR_NEWOBJ_WEIGHT;
extern float LIDAR_RX_GATE;
extern float LIDAR_RY_GATE;
extern SensorFusion fusion_tracker;

LidarTracker::LidarTracker(void)
{
    matched_pair.clear();
    prev_matched.clear();
    src_matched.clear();
    track_info.clear();
    X.clear();
    X_.clear();
    P.clear();
    P_.clear();
    
    init_P = matrix6d::Zero(6,6);
    init_P(0,0) = 1;    init_P(1,1) = 1;
    init_P(2,2) = 4;    init_P(3,3) = 4;
    init_P(4,4) = 9;    init_P(5,5) = 9;
    F = matrix6d::Zero(6,6);
    F(0,0) = 1;         F(1,1) = 1;
    F(2,2) = 1;         F(3,3) = 1;
    F(4,4) = 1;         F(5,5) = 1;
    Q = matrix6d::Zero(6,6);
    Q(0,0) = 0.001;     Q(1,1) = 0.001;
    Q(2,2) = 0.05;      Q(3,3) = 0.05;
    Q(4,4) = 0.1;       Q(5,5) = 0.1;
    H = matrix2_6d::Zero(2,6);
    H(0,0) = 1;
    H(1,1) = 1;
    R = matrix2d::Zero(2,2);
    R(0,0) = 0.0225;       // 0.15^2
    R(1,1) = 0.0225;       // 0.15^2
}

LidarTracker::~LidarTracker() { }

void LidarTracker::KF(const detection::LidarRawArray& input)
{
    // clock_t start = clock();
    std::vector<LidarObject> src;
    LidarObject raw;
    for(int i=0; i<input.num; ++i){
        raw.rx = input.data[i].x;
        raw.ry = input.data[i].y;
        raw.width = input.data[i].width;
        src.push_back(raw);
    }

    if (!X.size())
    {
      for (auto &obj : src)
      {
          InitTrack(obj);
      }
      time_stamp = input.header.stamp;
    }
    else
    {
      ts = (input.header.stamp - time_stamp).toSec();
      time_stamp = input.header.stamp;
      Predict();
      MatchGNN(src);
      Update(src);
    }
    // clock_t end = clock();
    // float duration_ms = (float)(end-start)*1000/(float)CLOCKS_PER_SEC;  //程序用时 ms
    // std::cout << "duration(ms) = " << duration_ms << std::endl;
    PubLidarTracks();
    fusion_tracker.Run();    //fusion point at lidar received time
}

void LidarTracker::InitTrack(const LidarObject &obj)
{
    vector6d init_X = vector6d::Zero(6);
    init_X(0) = obj.rx;
    init_X(1) = obj.ry;
    X.push_back(init_X);
    X_.push_back(init_X);
    P.push_back(init_P);
    P_.push_back(init_P);

    ObjectInfo init_info(UNKNOWN, obj.width);
    track_info.push_back(init_info);
}

void LidarTracker::Predict(void)
{
    if(X.size() != P.size()){
      ROS_ERROR("lidar tracker error: Predict state size not equal");
    }
    F(0,2) = F(1,3) = F(2,4) = F(3,5) = ts;
    F(0,4) = F(1,5) = ts*ts/2;
    int prev_track_num = X.size();
    for (int i=0; i<prev_track_num; ++i)
    {
        X[i] = F * X[i];
        P[i] = F * P[i] * F.transpose() + Q;
        X_[i] = X[i];
        P_[i] = P[i];
    }
}

void LidarTracker::MatchGNN(const std::vector<LidarObject>& src)
{
    int prev_track_num = X.size();
    int src_obj_num = src.size();

    matched_pair.clear();
    src_matched.clear();
    src_matched.resize(src_obj_num, false);
    prev_matched.clear();
    prev_matched.resize(prev_track_num, false);

    matrixXd w_ij = matrixXd::Zero(src_obj_num, prev_track_num + src_obj_num);

    // get likelihoods of measurements within track pdfs
    for ( int i = 0; i < src_obj_num; ++i )
    {
        float rx = src[i].rx;
        float ry = src[i].ry;
        vector2d z(rx, ry);
        for ( int j = 0; j < prev_track_num; ++j ){
            float rx_ = X[j](0);
            float ry_ = X[j](1);
            if (fabs(rx - rx_) < LIDAR_RX_GATE && fabs(ry - ry_) < LIDAR_RY_GATE) // track gate
            {
                vector2d z_(rx_, ry_);
                matrix2d S = H * P[j] * H.transpose() + R;
                w_ij(i, j) = normalDistributionDensity< 2 >(S, z_, z);
                // std::cout << "w_ij(i, j) = " << w_ij(i, j) <<std::endl;
            }else{
                w_ij(i, j) = 0;
            }
        }
    }

    // weights for initializing new filters
    for ( int j = prev_track_num; j < prev_track_num + src_obj_num; ++j ){
        w_ij(j - prev_track_num, j) = LIDAR_NEWOBJ_WEIGHT;
    }

    // solve the maximum-sum-of-weights problem (i.e. assignment problem)
    // in this case it is global nearest neighbour by minimizing the distances
    // over all measurement-filter-associations
    Auction<double>::Edges assignments = Auction<double>::solve(w_ij);

    // for all found assignments
    for ( const auto & e : assignments )
    {
        // is assignment an assignment from an already existing filter to a measurement?
        if ( e.y < prev_track_num )
        {
            std::pair<int, int> pair(e.x, e.y);  // (measurement, predict)
            matched_pair.push_back(pair);
            src_matched[e.x] = true;
            prev_matched[e.y] = true;

            track_info[e.y].confi_dec = 0;    // target matched, confidence increase
            track_info[e.y].confi_inc++;
            track_info[e.y].confidence += log(track_info[e.y].confi_inc + 1) / log(1.5f);
            if (track_info[e.y].confidence > LIDAR_MAX_CONFIDENCE) track_info[e.y].confidence = LIDAR_MAX_CONFIDENCE;
        }
        else // is this assignment a measurement that is considered new?
        {
            // create filter with measurment and keep it
            InitTrack(src[e.x]);
        }
    }

    for (int i=prev_track_num-1; i>=0; --i)  // from back to front to avoid wrongly removing
    {
        if (!prev_matched[i])
        {
            track_info[i].confi_inc = 0;    // target not matched, confidence decrease
            track_info[i].confi_dec++;
            track_info[i].confidence -= pow(1.5f, track_info[i].confi_dec);

            if(track_info[i].confidence < 0 || !IsConverged(i))    // remove lost target
            {
                // std::cout <<"remove"<<std::endl;
                // std::cout<< P[i](0,0)<<" "<<P[i](1,1) <<" "<<P[i](2,2) <<" "<<P[i](3,3) <<" "<<P[i](4,4) <<" "<<P[i](5,5) <<std::endl;
                RemoveTrack(i);
            }
        }
    }
}

void LidarTracker::Update(const std::vector<LidarObject>& src)
{
    if(X.size() != P.size()){
      ROS_ERROR("lidar tracker error: Update state size not equal");
    }
    
    for (int i=0; i<matched_pair.size(); ++i)    // upgrade matched
    {
        int src_idx = matched_pair[i].first;
        int prev_idx = matched_pair[i].second;

        float rx_ = X[prev_idx](0);
        float ry_ = X[prev_idx](1);

        float rx = src[src_idx].rx;
        float ry = src[src_idx].ry;

        vector2d Y(rx-rx_, ry-ry_);
        matrix2d S = H * P[prev_idx] * H.transpose() + R;
        matrix6_2d K = matrix6_2d::Zero(6,2);
        K = P[prev_idx] * H.transpose() * S.inverse();

        X[prev_idx] = X[prev_idx] + K * Y;
        P[prev_idx] = (matrix6d::Identity(6,6) - K * H) * P[prev_idx];
    }
}

template <class T>
static void erase_from_vector(std::vector<T> &v, int index)
{
    if(index >= v.size()){
      ROS_ERROR("lidar tracker error: remove track index >= size");
      return;
    }
    v.erase(v.begin() + index);
}

void LidarTracker::RemoveTrack(int index)
{
    erase_from_vector(X, index);
    erase_from_vector(X_, index);
    erase_from_vector(P, index);
    erase_from_vector(P_, index);
    erase_from_vector(track_info, index);
}

bool LidarTracker::IsConverged(int track_index)
{
    bool converged = false;
    float rx_cov = P[track_index](0,0);
    float ry_cov = P[track_index](1,1);
    float vx_cov = P[track_index](2,2);
    float vy_cov = P[track_index](3,3);
    float ax_cov = P[track_index](4,4);
    float ay_cov = P[track_index](5,5);
    if (rx_cov < 5 && ry_cov < 5 
        && vx_cov < 25 && vy_cov < 25 && ax_cov < 25 && ay_cov < 25)
    {
        converged = true;
    }
    return converged;
}

void LidarTracker::PubLidarTracks(void)
{
    static int pre_marker_size_ = 0;
    visualization_msgs::MarkerArray marker_array;
    visualization_msgs::Marker bbox_marker;
    bbox_marker.header.frame_id = FIXED_FRAME;
    bbox_marker.header.stamp = time_stamp;
    bbox_marker.color.r = 0.0f;
    bbox_marker.color.g = 1.0f;    //lidar color green
    bbox_marker.color.b = 0.0f;
    bbox_marker.color.a = 0.5f;
    bbox_marker.lifetime = ros::Duration();
    bbox_marker.frame_locked = true;
    bbox_marker.type = visualization_msgs::Marker::CUBE;
    bbox_marker.action = visualization_msgs::Marker::ADD;
    detection::TargetArray target_array;
    detection::Target target;
    target_array.header.stamp = time_stamp;

    int marker_id = 0;
    int track_num = X.size();
    for (int i=0; i<track_num; ++i)
    {
        if(track_info[i].confidence < LIDAR_MIN_CONFIDENCE) continue;
        if (!IsConverged(i)){
            // std::cout <<"not display"<<std::endl;
            // std::cout<< P[i](0,0)<<" "<<P[i](1,1) <<" "<<P[i](2,2) <<" "<<P[i](3,3) <<" "<<P[i](4,4) <<" "<<P[i](5,5) <<std::endl;
            // std::cout<< X[i](4)<<" "<<X[i](5)<<std::endl;
            continue;
        }  

        bbox_marker.id = marker_id;
        bbox_marker.pose.position.x = X[i](0);
        bbox_marker.pose.position.y = X[i](1);
        bbox_marker.pose.position.z = -0.9;
        bbox_marker.scale.x = PED_WIDTH;
        bbox_marker.scale.y = track_info[i].width;
        bbox_marker.scale.z = PED_HEIGHT;
        marker_array.markers.push_back(bbox_marker);
        target.rx = X[i](0);
        target.ry = X[i](1);
        target.vx = X[i](2);
        target.vy = X[i](3);
        target.ax = X[i](4);
        target.ay = X[i](5);
        target.rx_cov = P[i](0,0);
        target.ry_cov = P[i](1,1);
        target.vx_cov = P[i](2,2);
        target.vy_cov = P[i](3,3);
        target.ax_cov = P[i](4,4);
        target.ay_cov = P[i](5,5);
        target.width  = track_info[i].width;
        target.type   = track_info[i].type;
        target_array.data[marker_id] = target;
        marker_id++;
    }
    target_array.num = marker_id;

    if (marker_array.markers.size() > pre_marker_size_)
    {
        pre_marker_size_ = marker_array.markers.size();
    }

    for (int i = marker_id; i < pre_marker_size_; ++i)
    {
        bbox_marker.id = i;
        bbox_marker.action = visualization_msgs::Marker::DELETE;
        marker_array.markers.push_back(bbox_marker);
    }
    pre_marker_size_ = marker_id;
    lidar_kf_pub.publish(marker_array);
    lidar_pub.publish(target_array);
}

void LidarTracker::GetTimeStamp(ros::Time& stamp){
    stamp = time_stamp;
}

void LidarTracker::GetLidarTrack(std::vector<LocalTrack>& tracks){
    tracks.clear();
    LocalTrack track;
    int size = X.size();
    for(int i=0; i<size; ++i){
        if(track_info[i].confidence < LIDAR_MIN_CONFIDENCE) continue;
        if (!IsConverged(i))  continue;
        track.X  = X[i];
        track.X_ = X_[i];
        track.P  = P[i];
        track.P_ = P_[i];
        track.width = track_info[i].width;
        tracks.push_back(track);
    }
}