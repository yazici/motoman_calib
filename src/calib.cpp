#include <ros/ros.h>
#include <ros/package.h>
#include <sensor_msgs/PointCloud2.h>


#include <vtkVersion.h>
#include <vtkPLYReader.h>
#include <vtkOBJReader.h>
#include <vtkTriangle.h>
#include <vtkTriangleFilter.h>
#include <vtkPolyDataMapper.h>
#include <pcl/io/io.h>
#include <pcl/io/pcd_io.h>
#include <pcl/io/vtk_lib_io.h>
#include <pcl/point_types.h>
#include <pcl/registration/icp.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/filters/passthrough.h>
#include <pcl/search/kdtree.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl_ros/impl/transforms.hpp>

const int sampling_points = 10000;

#define gazebo

inline double
uniform_deviate (int seed)
{
  double ran = seed * (1.0 / (RAND_MAX + 1.0));
  return ran;
}

inline void
randomPointTriangle (float a1, float a2, float a3, float b1, float b2, float b3, float c1, float c2, float c3,
                     Eigen::Vector4f& p)
{
  float r1 = static_cast<float> (uniform_deviate (rand ()));
  float r2 = static_cast<float> (uniform_deviate (rand ()));
  float r1sqr = sqrtf (r1);
  float OneMinR1Sqr = (1 - r1sqr);
  float OneMinR2 = (1 - r2);
  a1 *= OneMinR1Sqr;
  a2 *= OneMinR1Sqr;
  a3 *= OneMinR1Sqr;
  b1 *= OneMinR2;
  b2 *= OneMinR2;
  b3 *= OneMinR2;
  c1 = r1sqr * (r2 * c1 + b1) + a1;
  c2 = r1sqr * (r2 * c2 + b2) + a2;
  c3 = r1sqr * (r2 * c3 + b3) + a3;
  p[0] = c1;
  p[1] = c2;
  p[2] = c3;
  p[3] = 0;
}

inline void
randPSurface (vtkPolyData * polydata, std::vector<double> * cumulativeAreas, double totalArea, Eigen::Vector4f& p)
{
  float r = static_cast<float> (uniform_deviate (rand ()) * totalArea);

  std::vector<double>::iterator low = std::lower_bound (cumulativeAreas->begin (), cumulativeAreas->end (), r);
  vtkIdType el = vtkIdType (low - cumulativeAreas->begin ());

  double A[3], B[3], C[3];
  vtkIdType npts = 0;
  vtkIdType *ptIds = NULL;
  polydata->GetCellPoints (el, npts, ptIds);
  polydata->GetPoint (ptIds[0], A);
  polydata->GetPoint (ptIds[1], B);
  polydata->GetPoint (ptIds[2], C);
  randomPointTriangle (float (A[0]), float (A[1]), float (A[2]), 
                       float (B[0]), float (B[1]), float (B[2]), 
                       float (C[0]), float (C[1]), float (C[2]), p);
}

void
uniform_sampling (vtkSmartPointer<vtkPolyData> polydata, size_t n_samples, pcl::PointCloud<pcl::PointXYZ> & cloud_out)
{
  polydata->BuildCells ();
  vtkSmartPointer<vtkCellArray> cells = polydata->GetPolys ();

  double p1[3], p2[3], p3[3], totalArea = 0;
  std::vector<double> cumulativeAreas (cells->GetNumberOfCells (), 0);
  size_t i = 0;
  vtkIdType npts = 0, *ptIds = NULL;
  for (cells->InitTraversal (); cells->GetNextCell (npts, ptIds); i++)
  {
    polydata->GetPoint (ptIds[0], p1);
    polydata->GetPoint (ptIds[1], p2);
    polydata->GetPoint (ptIds[2], p3);
    totalArea += vtkTriangle::TriangleArea (p1, p2, p3);
    cumulativeAreas[i] = totalArea;
  }

  cloud_out.points.resize (n_samples);
  cloud_out.width = static_cast<pcl::uint32_t> (n_samples);
  cloud_out.height = 1;

  for (i = 0; i < n_samples; i++)
  {
    Eigen::Vector4f p;
    randPSurface (polydata, &cumulativeAreas, totalArea, p);
    cloud_out.points[i].x = p[0];
    cloud_out.points[i].y = p[1];
    cloud_out.points[i].z = p[2];
  }
}

class MotomanMeshCloud
{
public:
  MotomanMeshCloud()
    : rate_(1), pcl_shifted_cloud_(new pcl::PointCloud<pcl::PointXYZ>())
  {
    link_names_.push_back("base_link.stl");
    frame_names_.push_back("/base_link");
    link_names_.push_back("link_s.stl");
    frame_names_.push_back("/link_s");
    link_names_.push_back("link_l.stl");
    frame_names_.push_back("/link_l");
    link_names_.push_back("link_e.stl");
    frame_names_.push_back("/link_e");
    link_names_.push_back("link_u.stl");
    frame_names_.push_back("/link_u");
    link_names_.push_back("link_r.stl");
    frame_names_.push_back("/link_r");
    link_names_.push_back("link_b.stl");
    frame_names_.push_back("/link_b");
    link_names_.push_back("link_t.stl");
    frame_names_.push_back("/link_t");

    corrected_cloud_frame_ = "kinect2_rgb_optical_frame";
    
    for(int i = 0; i < link_names_.size(); ++i){
      this->getMesh(ros::package::getPath("motoman_description")+"/meshes/sia5/collision/STL/"+link_names_[i], frame_names_[i]);
    }
    
    this->transformMesh();
    
    mesh_pointcloud_publisher_ = nh_.advertise<sensor_msgs::PointCloud2>("/mesh_cloud", 1);
    shifted_cloud_publisher_ = nh_.advertise<sensor_msgs::PointCloud2>("/shifted_cloud",1);
    corrected_cloud_publisher_ = nh_.advertise<sensor_msgs::PointCloud2>("/corrected_cloud",1);
    kinect_subscriber_ = nh_.subscribe<sensor_msgs::PointCloud2>("/kinect2/hd/points", 10, boost::bind(&MotomanMeshCloud::pointCloudCallback, this, _1));
  }
  ~MotomanMeshCloud()
  {
  }
  void getMesh(std::string dir_path, std::string frame_name)
  {
    pcl::PolygonMesh mesh;
    pcl::io::loadPolygonFileSTL(dir_path, mesh);
    vtkSmartPointer<vtkPolyData> polydata1 = vtkSmartPointer<vtkPolyData>::New();;
    pcl::io::mesh2vtk(mesh, polydata1);
    pcl::PointCloud<pcl::PointXYZ>::Ptr parts_cloud(new pcl::PointCloud<pcl::PointXYZ>);
    uniform_sampling (polydata1, sampling_points, *parts_cloud);
    parts_clouds_.push_back(*parts_cloud);
    
    //pcl_conversions::fromPCL(*cloud_1, mesh_pointcloud_);
  }

  void transformMesh()
  {
    pcl::PointCloud<pcl::PointXYZ> transformed_parts_cloud;
    for (int retryCount = 0; retryCount < 10; ++retryCount) {
      for(size_t i = 0; i< parts_clouds_.size(); ++i){
        try{
          tf::StampedTransform transform;
          tf_.lookupTransform("/world", frame_names_[i],
                              ros::Time(0), transform);
          pcl_ros::transformPointCloud(parts_clouds_[i],transformed_parts_cloud, transform);
          sia5_cloud_ += transformed_parts_cloud;
          ROS_INFO_STREAM("Get transform : " << frame_names_[i]);
        }catch(tf2::LookupException e)
        {
          ROS_ERROR("pcl::ros %s",e.what());
          ros::Duration(1.0).sleep();
          sia5_cloud_.clear();
          break;
        }catch(tf2::ExtrapolationException e)
        {
          ROS_ERROR("pcl::ros %s",e.what());
          ros::Duration(1.0).sleep();
          sia5_cloud_.clear();
          break;
        }catch(...)
        {
          ros::Duration(1.0).sleep();
          sia5_cloud_.clear();
          break;
        }
      }
      if(sia5_cloud_.points.size() == (sampling_points*parts_clouds_.size())){
        std::cout << "0) mesh : " << sia5_cloud_.points.size() << std::endl;
        return;
      }
    }
    ROS_WARN_STREAM("try tf transform 5times, but failed");
    exit(-1);
    return;
  }
  
  void cropBox(pcl::PointCloud<pcl::PointXYZ>::Ptr cloud,
               pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_filtered)
  {
    pcl::PassThrough<pcl::PointXYZ> passthrough_filter;
    passthrough_filter.setInputCloud(cloud);
    passthrough_filter.setFilterFieldName("z");
    passthrough_filter.setFilterLimits(-0.1, 0.03);
    passthrough_filter.setFilterLimitsNegative (true);
    passthrough_filter.filter (*cloud);
    
    pcl::search::KdTree<pcl::PointXYZ> kdtree;
    kdtree.setInputCloud(cloud);
    pcl::PointXYZ search_point;
    search_point.x = 0;
    search_point.y = 0;
    search_point.z = 0;
    double search_radius = 1.0;
    std::vector<float> point_radius_squared_distance;
    pcl::PointIndices::Ptr point_idx_radius_search(new pcl::PointIndices());

    if ( kdtree.radiusSearch (search_point, search_radius, point_idx_radius_search->indices, point_radius_squared_distance) > 0 )
    {
      pcl::ExtractIndices<pcl::PointXYZ> extractor;
      extractor.setInputCloud(cloud);
      extractor.setIndices(point_idx_radius_search);
      extractor.setNegative(false);
      extractor.filter(*cloud_filtered);
    }
  }

  pcl::PointCloud<pcl::PointXYZ> shiftPointCloud(pcl::PointCloud<pcl::PointXYZ> points, double x, double y, double z, double roll, double pitch, double yaw)
  {
    tf::Matrix3x3 init_rotation;
    Eigen::AngleAxisf rotation_x(roll, Eigen::Vector3f::UnitX());
    Eigen::AngleAxisf rotation_y(pitch, Eigen::Vector3f::UnitY());
    Eigen::AngleAxisf rotation_z(yaw, Eigen::Vector3f::UnitZ());
    Eigen::Translation3f translation(x, y, z);
    Eigen::Matrix4f noize_matrix = (translation * rotation_z * rotation_y * rotation_x).matrix();
    pcl::PointCloud<pcl::PointXYZ> shifted_cloud;
    pcl::transformPointCloud(points, shifted_cloud, noize_matrix);
    return shifted_cloud;
  }
  
  void pointCloudCallback(const sensor_msgs::PointCloud2::ConstPtr& kinect_pointcloud)
  {
    Eigen::Matrix4f t(Eigen::Matrix4f::Identity());

    // ROSのメッセージからPCLの形式に変換
    pcl::PointCloud<pcl::PointXYZ>::Ptr pcl_pc(new pcl::PointCloud<pcl::PointXYZ>());
    pcl::fromROSMsg(*kinect_pointcloud, *pcl_pc);
    // デバッグのためにわざとずらす
    #ifdef gazebo
    *pcl_shifted_cloud_ = this->shiftPointCloud(*pcl_pc,
                                                0.1, 0.1, 0, 0.01, 0.01, 0.01);
    #else
    *pcl_shifted_cloud_ = *pcl_pc;
    #endif
    // world 座標系に変換
    pcl_ros::transformPointCloud("/world", *pcl_shifted_cloud_, *pcl_shifted_cloud_, tf_);
    this->cropBox(pcl_shifted_cloud_, pcl_shifted_cloud_);
    sensor_msgs::PointCloud2 ros_shifted_cloud;
    pcl::toROSMsg(*pcl_shifted_cloud_, ros_shifted_cloud);
    ros_shifted_cloud.header.stamp = ros::Time::now();
    ros_shifted_cloud.header.frame_id = "/world";//kinect_pointcloud->header.frame_id;
    corrected_cloud_frame_ = "/world";//kinect_pointcloud->header.frame_id;
    shifted_cloud_publisher_.publish(ros_shifted_cloud); // デバッグのためにずらしたやつのpublish
  }

  void run()
  {
    while(ros::ok())
    {
      pcl::toROSMsg(sia5_cloud_, mesh_pointcloud_);
      mesh_pointcloud_.header.stamp = ros::Time::now();
      mesh_pointcloud_.header.frame_id = "/world";
      mesh_pointcloud_publisher_.publish(mesh_pointcloud_);

      pcl::IterativeClosestPoint<pcl::PointXYZ, pcl::PointXYZ> icp;

      pcl::PointCloud<pcl::PointXYZ>::Ptr sia5_ptr(new pcl::PointCloud<pcl::PointXYZ>(sia5_cloud_));
      std::vector<int> nan_index;
      pcl::removeNaNFromPointCloud(*pcl_shifted_cloud_, *pcl_shifted_cloud_, nan_index);
      pcl::removeNaNFromPointCloud(*sia5_ptr, *sia5_ptr, nan_index);
      icp.setInputSource(pcl_shifted_cloud_);
      icp.setInputTarget(sia5_ptr);
      icp.setMaximumIterations(1000);
      pcl::PointCloud<pcl::PointXYZ> Final;
      icp.align(Final);
      sensor_msgs::PointCloud2 ros_corrected_cloud;
      pcl::toROSMsg(Final, ros_corrected_cloud);
      ros_corrected_cloud.header.stamp = ros::Time::now();
      ros_corrected_cloud.header.frame_id = "/world";
      corrected_cloud_publisher_.publish(ros_corrected_cloud);

      ros::spinOnce();
      rate_.sleep();
    }
  }
private:
  std::vector<pcl::PolygonMesh> meshes_;
  sensor_msgs::PointCloud2 mesh_pointcloud_;
  ros::NodeHandle nh_;
  tf::TransformListener tf_;
  ros::Rate rate_;
  ros::Publisher mesh_pointcloud_publisher_;
  ros::Publisher shifted_cloud_publisher_;
  ros::Publisher corrected_cloud_publisher_;
  ros::Subscriber kinect_subscriber_;
  std::vector<std::string> link_names_;
  std::vector<std::string> frame_names_;
  pcl::PointCloud<pcl::PointXYZ> sia5_cloud_;
  std::vector<pcl::PointCloud<pcl::PointXYZ> >parts_clouds_;
  sensor_msgs::PointCloud2 kinect_pointcloud_;
  pcl::PointCloud<pcl::PointXYZ>::Ptr pcl_shifted_cloud_;
  std::string corrected_cloud_frame_;
};


int main(int argc, char *argv[])
{
  ros::init(argc, argv, "motoman_calib");
  MotomanMeshCloud mesh_cloud;
  mesh_cloud.run();
  return 0;
}