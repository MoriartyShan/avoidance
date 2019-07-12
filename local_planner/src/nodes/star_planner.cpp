#include "local_planner/star_planner.h"

#include "avoidance/common.h"
#include "local_planner/planner_functions.h"
#include "local_planner/tree_node.h"

#include <ros/console.h>

namespace avoidance {

StarPlanner::StarPlanner() : tree_age_(0) {}

// set parameters changed by dynamic rconfigure
void StarPlanner::dynamicReconfigureSetStarParams(const avoidance::LocalPlannerNodeConfig& config, uint32_t level) {
  children_per_node_ = config.children_per_node_;
  n_expanded_nodes_ = config.n_expanded_nodes_;
  tree_node_distance_ = static_cast<float>(config.tree_node_distance_);
  max_path_length_ = static_cast<float>(config.box_radius_);
  smoothing_margin_degrees_ = static_cast<float>(config.smoothing_margin_degrees_);
}

void StarPlanner::setParams(costParameters cost_params) { cost_params_ = cost_params; }

void StarPlanner::setLastDirection(const Eigen::Vector3f& projected_last_wp) { projected_last_wp_ = projected_last_wp; }

void StarPlanner::setPose(const Eigen::Vector3f& pos, const Eigen::Vector3f& vel, float curr_yaw_fcu_frame_deg) {
  position_ = pos;
  velocity_ = vel;
  curr_yaw_histogram_frame_deg_ = wrapAngleToPlusMinus180(-curr_yaw_fcu_frame_deg + 90.0f);
}

void StarPlanner::setGoal(const Eigen::Vector3f& goal) {
  goal_ = goal;
  tree_age_ = 1000;
}

void StarPlanner::setPointcloud(const pcl::PointCloud<pcl::PointXYZI>& cloud) { cloud_ = cloud; }

float StarPlanner::treeHeuristicFunction(int node_number) const {
  return (goal_ - tree_[node_number].getPosition()).norm();
}

void StarPlanner::buildLookAheadTree() {
  std::clock_t start_time = std::clock();

  Histogram histogram(ALPHA_RES);
  std::vector<uint8_t> cost_image_data;
  std::vector<candidateDirection> candidate_vector;
  Eigen::MatrixXf cost_matrix;

  bool is_expanded_node = true;

  tree_.clear();
  closed_set_.clear();

  // insert first node
  tree_.push_back(TreeNode(0, 0, position_, velocity_));
  tree_.back().setCosts(treeHeuristicFunction(0), treeHeuristicFunction(0));
  tree_.back().yaw_ = curr_yaw_histogram_frame_deg_;
  tree_.back().last_z_ = tree_.back().yaw_;

  int origin = 0;
  for (int n = 0; n < n_expanded_nodes_ && is_expanded_node; n++) {
    Eigen::Vector3f origin_position = tree_[origin].getPosition();
    Eigen::Vector3f origin_velocity = tree_[origin].getVelocity();
    PolarPoint facing_goal = cartesianToPolarHistogram(goal_, origin_position);
    float distance_to_goal = (goal_ - origin_position).norm();

    histogram.setZero();
    generateNewHistogram(histogram, cloud_, origin_position);

    // calculate candidates
    cost_matrix.fill(0.f);
    cost_image_data.clear();
    candidate_vector.clear();
    getCostMatrix(histogram, goal_, origin_position, origin_velocity, cost_params_, smoothing_margin_degrees_,
                  cost_matrix, cost_image_data);
    getBestCandidatesFromCostMatrix(cost_matrix, children_per_node_, candidate_vector);

    // add candidates as nodes
    if (candidate_vector.empty()) {
      tree_[origin].total_cost_ = HUGE_VAL;
    } else {
      // insert new nodes
      int depth = tree_[origin].depth_ + 1;
      int children = 0;
      for (candidateDirection candidate : candidate_vector) {
        PolarPoint candidate_polar = candidate.toPolar(tree_node_distance_);

        // check if another close node has been added
        Eigen::Vector3f node_location = polarHistogramToCartesian(candidate_polar, origin_position);
        Eigen::Vector3f node_velocity =
            tree_[tree_[origin].origin_].getVelocity() + (node_location - origin_position);  // todo: simulate!
        int close_nodes = 0;
        for (size_t i = 0; i < tree_.size(); i++) {
          float dist = (tree_[i].getPosition() - node_location).norm();
          if (dist < 0.2f) {
            close_nodes++;
            break;
          }
        }

        if (children < children_per_node_ && close_nodes == 0) {
          tree_.push_back(TreeNode(origin, depth, node_location, node_velocity));
          tree_.back().last_e_ = candidate_polar.e;
          tree_.back().last_z_ = candidate_polar.z;  // still needed?
          float h = treeHeuristicFunction(tree_.size() - 1);
          float distance_cost = 0.f, other_cost = 0.f;  // dummy placeholders
          Eigen::Vector2i idx_ppol = polarToHistogramIndex(candidate_polar, ALPHA_RES);
          float obstacle_distance = histogram.get_dist(idx_ppol.x(), idx_ppol.y());
          float c = costFunction(candidate_polar, obstacle_distance, goal_, node_location, node_velocity, cost_params_,
                                 distance_cost, other_cost);
          tree_.back().heuristic_ = h;
          tree_.back().total_cost_ = tree_[origin].total_cost_ - tree_[origin].heuristic_ + c + h;
          Eigen::Vector3f diff = node_location - origin_position;
          float yaw_radians = atan2(diff.y(), diff.x());
          tree_.back().yaw_ = std::round((-yaw_radians * 180.0f / M_PI_F)) + 90.0f;  // still needed?
          children++;
        }
      }
    }

    closed_set_.push_back(origin);
    tree_[origin].closed_ = true;

    // find best node to continue
    float minimal_cost = HUGE_VAL;
    is_expanded_node = false;
    for (size_t i = 0; i < tree_.size(); i++) {
      if (!(tree_[i].closed_)) {
        float node_distance = (tree_[i].getPosition() - position_).norm();
        if (tree_[i].total_cost_ < minimal_cost && node_distance < max_path_length_) {
          minimal_cost = tree_[i].total_cost_;
          origin = i;
          is_expanded_node = true;
        }
      }
    }

    cost_image_data.clear();
    candidate_vector.clear();
  }
  // smoothing between trees
  int tree_end = origin;
  path_node_positions_.clear();
  path_node_origins_.clear();
  while (tree_end > 0) {
    path_node_origins_.push_back(tree_end);
    path_node_positions_.push_back(tree_[tree_end].getPosition());
    tree_end = tree_[tree_end].origin_;
  }
  path_node_positions_.push_back(tree_[0].getPosition());
  path_node_origins_.push_back(0);
  tree_age_ = 0;

  ROS_INFO("\033[0;35m[SP]Tree (%lu nodes, %lu path nodes, %lu expanded) calculated in %2.2fms.\033[0m", tree_.size(),
           path_node_positions_.size(), closed_set_.size(),
           static_cast<double>((std::clock() - start_time) / static_cast<double>(CLOCKS_PER_SEC / 1000)));
  for (int j = 0; j < path_node_positions_.size(); j++) {
    ROS_DEBUG("\033[0;35m[SP] node %i : [ %f, %f, %f]\033[0m", j, path_node_positions_[j].x(),
              path_node_positions_[j].y(), path_node_positions_[j].z());
  }
}
}
