// Copyright (c) 2023 Norwegian University of Life Sciences, Fetullah Atas
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#ifndef VOX_NAV_CONTROL__PLAN_REFINER_PLUGINS__TRAVERSABILITY_BASED_PLAN_REFINER_HPP_
#define VOX_NAV_CONTROL__PLAN_REFINER_PLUGINS__TRAVERSABILITY_BASED_PLAN_REFINER_HPP_
#pragma once

#include <string>
#include <memory>
#include <vector>
#include <boost/graph/astar_search.hpp>
#include <boost/graph/dijkstra_shortest_paths.hpp>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/random.hpp>
#include <boost/random.hpp>
#include <boost/graph/graphviz.hpp>

#include <fcl/config.h>
#include <fcl/geometry/octree/octree.h>
#include <fcl/math/constants.h>
#include <fcl/narrowphase/collision.h>
#include <fcl/narrowphase/collision_object.h>

#include <tf2_ros/transform_listener.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <tf2_ros/buffer.h>
#include <pcl_ros/transforms.hpp>
#include <pcl_conversions/pcl_conversions.h>

#include <sensor_msgs/msg/point_cloud2.hpp>
#include <visualization_msgs/msg/marker_array.hpp>
#include <nav_msgs/msg/path.hpp>
#include <vision_msgs/msg/detection3_d.hpp>

#include "vox_nav_control/common.hpp"
#include "vox_nav_control/plan_refiner_core.hpp"
#include "vox_nav_utilities/pcl_helpers.hpp"
#include "vox_nav_utilities/planner_helpers.hpp"
#include "vox_nav_utilities/map_manager_helpers.hpp"

namespace vox_nav_control
{

  class TraversabilityBasedPlanRefiner : public vox_nav_control::PlanRefinerCore
  {
  public:
    /**
     * @brief Construct a new Cam Based Plan Refiner object
     *
     */
    TraversabilityBasedPlanRefiner();

    /**
     * @brief Destroy the Cam Based Plan Refiner object
     *
     */
    ~TraversabilityBasedPlanRefiner();

    /**
     * @brief
     *
     * @param parent rclcpp node
     * @param plugin_name refiner plugin name
     */
    void initialize(
      rclcpp::Node * parent,
      const std::string & plugin_name) override;

    /**
     * @brief Refine the plan locally, only in the vicinity of the robot
     *
     * @param curr_pose
     * @param plan
     * @return std::vector<geometry_msgs::msg::PoseStamped>
     */
    bool refinePlan(
      const geometry_msgs::msg::PoseStamped & curr_pose,
      nav_msgs::msg::Path & plan_to_refine) override;

    /**
     * @brief Subscribes to traversability map topic and stores it in a member variable
     *
     * @param msg
     */
    void traversabilityMapCallback(const sensor_msgs::msg::PointCloud2::SharedPtr msg);

    /**
     * @brief Subscribes to traversability marker topic and stores it in a member variable
     *
     * @param msg
     */
    void traversabilityMarkerCallback(const visualization_msgs::msg::MarkerArray::SharedPtr msg);

  private:
    rclcpp::Node * node_;
    std::string plugin_name_;

    std::mutex global_mutex_;
    std::string map_topic_;
    bool is_enabled_;

    std::string traversability_layer_name_;
    double traversability_threshold_;

    // Subscribe to image topic
    // Subscribe to camera info topic
    rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr traversability_map_subscriber_;
    rclcpp::Subscription<visualization_msgs::msg::MarkerArray>::SharedPtr
      traversability_marker_subscriber_;
    rclcpp::Publisher<geometry_msgs::msg::PoseStamped>::SharedPtr local_goal_publisher_;
    rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr local_optimal_path_publisher_;
    rclcpp::Publisher<vision_msgs::msg::Detection3D>::SharedPtr traversability_map_bbox_publisher_;

    sensor_msgs::msg::PointCloud2::SharedPtr traversability_map_;
    visualization_msgs::msg::MarkerArray::SharedPtr traversability_marker_;


    // Project the plan to the image plane with tf and camera info
    std::shared_ptr<tf2_ros::Buffer> tf_buffer_ptr_;
    std::shared_ptr<tf2_ros::TransformListener> tf_listener_ptr_;

    // Boost graph for traversability map based plan refinement
    struct VertexProperty
    {
      std::uint32_t label;
      std::string name;
      pcl::PointXYZRGBA point;
    };
    typedef float Cost;
    // specify some types
    typedef boost::adjacency_list<
        boost::setS,            // edge
        boost::vecS,            // vertex
        boost::undirectedS,     // type
        VertexProperty,         // vertex property
        boost::property<boost::edge_weight_t, Cost>> // edge property
      GraphT;
    typedef boost::property_map<GraphT, boost::edge_weight_t>::type WeightMap;
    typedef GraphT::vertex_descriptor vertex_descriptor;
    typedef GraphT::edge_descriptor edge_descriptor;
    typedef GraphT::vertex_iterator vertex_iterator;
    typedef std::pair<int, int> edge;

    // euclidean distance heuristic
    template<class Graph, class CostType, class SuperVoxelClustersPtr>
    class distance_heuristic : public boost::astar_heuristic<Graph, CostType>
    {
    public:
      typedef typename boost::graph_traits<Graph>::vertex_descriptor Vertex;
      distance_heuristic(SuperVoxelClustersPtr sc, Vertex goal_vertex, Graph g)
      : supervoxel_clusters_(sc), goal_vertex_(goal_vertex), g_(g)
      {
      }
      CostType operator()(Vertex u)
      {
        auto u_vertex_label = g_[u].label;
        auto goal_vertex_label = g_[goal_vertex_].label;
        auto u_supervoxel_centroid = supervoxel_clusters_->at(u_vertex_label)->centroid_;
        auto goal_supervoxel_centroid = supervoxel_clusters_->at(goal_vertex_label)->centroid_;
        CostType dx = u_supervoxel_centroid.x - goal_supervoxel_centroid.x;
        CostType dy = u_supervoxel_centroid.y - goal_supervoxel_centroid.y;
        CostType dz = u_supervoxel_centroid.z - goal_supervoxel_centroid.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
      }

    private:
      SuperVoxelClustersPtr supervoxel_clusters_;
      Vertex goal_vertex_;
      Graph g_;
    };

    // exception for termination
    struct FoundGoal {};
    template<class Vertex>
    class custom_goal_visitor : public boost::default_astar_visitor
    {
    public:
      custom_goal_visitor(Vertex goal_vertex, int * num_visits)
      : goal_vertex_(goal_vertex), num_visits_(num_visits)
      {
      }
      template<class Graph>
      void examine_vertex(Vertex u, Graph & g)
      {
        ++(*num_visits_);
        if (u == goal_vertex_) {
          throw FoundGoal();
        }
      }

    private:
      Vertex goal_vertex_;
      int * num_visits_;
    };

    GraphT g_;
    WeightMap weightmap_;

    // Visualize supervoxel graph
    rclcpp::Publisher<visualization_msgs::msg::MarkerArray>::SharedPtr supervoxel_graph_publisher_;
    rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr supervoxel_clusters_publisher_;

    typedef std::map<std::uint32_t, pcl::Supervoxel<pcl::PointXYZRGBA>::Ptr> SuperVoxelClusters;
    SuperVoxelClusters supervoxel_clusters_;

    bool supervoxel_disable_transform_;
    float supervoxel_resolution_;
    float supervoxel_seed_resolution_;
    float supervoxel_color_importance_;
    float supervoxel_spatial_importance_;
    float supervoxel_normal_importance_;

    void fillSuperVoxelMarkersfromAdjacency(
      const std::map<std::uint32_t, pcl::Supervoxel<pcl::PointXYZRGBA>::Ptr> & supervoxel_clusters,
      const std::multimap<std::uint32_t, std::uint32_t> & supervoxel_adjacency,
      const std_msgs::msg::Header & header,
      visualization_msgs::msg::MarkerArray & marker_array)
    {
      int index = 0;
      // To make a graph of the supervoxel adjacency,
      // we need to iterate through the supervoxel adjacency multimap
      for (auto label_itr = supervoxel_adjacency.cbegin();
        label_itr != supervoxel_adjacency.cend(); )
      {
        // First get the label
        std::uint32_t supervoxel_label = label_itr->first;
        // Now get the supervoxel corresponding to the label
        auto supervoxel = supervoxel_clusters.at(supervoxel_label);

        visualization_msgs::msg::Marker line_strip;
        line_strip.header = header;
        line_strip.ns = "supervoxel_markers_ns";
        line_strip.id = index;
        line_strip.type = visualization_msgs::msg::Marker::LINE_STRIP;
        line_strip.action = visualization_msgs::msg::Marker::ADD;
        line_strip.lifetime = rclcpp::Duration::from_seconds(0.5);
        line_strip.scale.x = 0.1;
        geometry_msgs::msg::Point point;
        point.x = supervoxel->centroid_.x;
        point.y = supervoxel->centroid_.y;
        point.z = supervoxel->centroid_.z;
        std_msgs::msg::ColorRGBA yellow_color;
        yellow_color.r = 1.0;
        yellow_color.g = 1.0;
        yellow_color.a = 0.4;
        line_strip.points.push_back(point);
        line_strip.colors.push_back(yellow_color);

        visualization_msgs::msg::Marker sphere;
        sphere.header = header;
        sphere.ns = "supervoxel_markers_ns";
        sphere.id = index + 10000;
        sphere.type = visualization_msgs::msg::Marker::SPHERE;
        sphere.action = visualization_msgs::msg::Marker::ADD;
        sphere.lifetime = rclcpp::Duration::from_seconds(0.5);
        sphere.pose.position = point;
        sphere.scale.x = 0.3;
        sphere.scale.y = 0.3;
        sphere.scale.z = 0.3;
        sphere.color.a = 1.0;
        sphere.color.g = 1.0;
        sphere.color.b = 1.0;

        for (auto adjacent_itr = supervoxel_adjacency.equal_range(supervoxel_label).first;
          adjacent_itr != supervoxel_adjacency.equal_range(supervoxel_label).second; ++adjacent_itr)
        {
          auto neighbor_supervoxel =
            supervoxel_clusters.at(adjacent_itr->second);

          geometry_msgs::msg::Point n_point;
          n_point.x = neighbor_supervoxel->centroid_.x;
          n_point.y = neighbor_supervoxel->centroid_.y;
          n_point.z = neighbor_supervoxel->centroid_.z;
          line_strip.points.push_back(n_point);
          line_strip.colors.push_back(yellow_color);
        }
        // Move iterator forward to next label
        label_itr = supervoxel_adjacency.upper_bound(supervoxel_label);
        index++;

        marker_array.markers.push_back(sphere);
        marker_array.markers.push_back(line_strip);
      }
    }

    bool find_astar_path(
      const GraphT & g,
      const WeightMap & weightmap,
      const vertex_descriptor start_vertex,
      const vertex_descriptor goal_vertex,
      std::list<vertex_descriptor> & shortest_path)
    {
      std::vector<vertex_descriptor> p(boost::num_vertices(g));
      std::vector<Cost> d(boost::num_vertices(g));
      std::vector<geometry_msgs::msg::PoseStamped> plan_poses;
      int num_visited_nodes = 0;
      try {
        if (supervoxel_clusters_.empty()) {
          RCLCPP_WARN(
            node_->get_logger(),
            "Empty supervoxel clusters! failed to find a valid path!");
          return false;
        }
        auto heuristic =
          distance_heuristic<GraphT, Cost, SuperVoxelClusters *>(
          &supervoxel_clusters_, goal_vertex, g);
        auto c_visitor =
          custom_goal_visitor<vertex_descriptor>(goal_vertex, &num_visited_nodes);
        // astar
        boost::astar_search_tree(
          g, start_vertex, heuristic /*only difference*/,
          boost::predecessor_map(&p[0]).distance_map(&d[0]).visitor(c_visitor));
        // If a path found exception will be thrown and code block here
        // Should not be eecuted. If code executed up until here,
        // A path was NOT found. Warn user about it
        RCLCPP_WARN(node_->get_logger(), "A* search failed to find a valid path!");
        return false;
      } catch (FoundGoal found_goal) {
        // Found a path to the goal, catch the exception
        for (vertex_descriptor v = goal_vertex;; v = p[v]) {
          shortest_path.push_front(v);
          if (p[v] == v) {break;}
        }
        return true;
      }
    }

    vertex_descriptor get_nearest(
      const GraphT & g,
      const pcl::PointXYZRGBA & point)
    {
      // Simple O(N) algorithm to find closest vertex to start and goal poses on boost::graph g
      double dist_min = INFINITY;
      vertex_descriptor nn_vertex;
      for (auto vd : boost::make_iterator_range(vertices(g))) {
        auto voxel_centroid = g[vd].point;
        auto dist_to_crr_voxel_centroid =
          vox_nav_utilities::PCLPointEuclideanDist<>(point, voxel_centroid);

        if (dist_to_crr_voxel_centroid < dist_min) {
          dist_min = dist_to_crr_voxel_centroid;
          nn_vertex = vd;
        }
      }
      return nn_vertex;
    }

    void fillCloudfromGraph(
      const GraphT & g,
      pcl::PointCloud<pcl::PointXYZRGBA>::Ptr cloud)
    {
      for (auto vd : boost::make_iterator_range(vertices(g))) {
        auto voxel_centroid = g[vd].point;
        pcl::PointXYZRGBA point;
        point.x = voxel_centroid.x;
        point.y = voxel_centroid.y;
        point.z = voxel_centroid.z;
        cloud->points.push_back(point);
      }
    }

  };

}      // namespace vox_nav_control


#endif  // VOX_NAV_CONTROL__PLAN_REFINER_PLUGINS__TRAVERSABILITY_BASED_PLAN_REFINER_HPP_
