#include <godel_process_planning/godel_process_planning.h>

#include <ros/console.h>

// descartes
#include "descartes_trajectory/axial_symmetric_pt.h"
#include "descartes_trajectory/joint_trajectory_pt.h"
#include "descartes_planner/dense_planner.h"

#include "common_utils.h"
#include "eigen_conversions/eigen_msg.h"
#include "boost/make_shared.hpp"

namespace godel_process_planning
{

// Planning Constants
const double BLENDING_ANGLE_DISCRETIZATION =
    M_PI / 12.0; // The discretization of the tool's pose about
                 // the z axis
const double FREE_SPACE_MAX_ANGLE_DELTA =
    M_PI_2; // The maximum angle a joint during a freespace motion
            // from the start to end position without that motion
            // being penalized. Avoids flips.
const double FREE_SPACE_ANGLE_PENALTY =
    5.0; // The factor by which a joint motion is multiplied if said
         // motion is greater than the max.
const static std::string JOINT_TOPIC_NAME =
    "joint_states"; // ROS topic to subscribe to for current robot
                    // state info

/**
 * @brief Translated an Eigen pose to a Descartes trajectory point appropriate for the BLEND
 * process!
 *        Note that this function is local only to this file, and there is a similar function in
 *        the keyence_process_planning.cpp document.
 * @param pose
 * @param dt The upper limit of time from the previous point to achieve this one
 * @return A descartes trajectory point encapsulating a move to this pose
 */
static inline descartes_core::TrajectoryPtPtr toDescartesPt(const Eigen::Affine3d& pose, double dt)
{
  using namespace descartes_trajectory;
  using namespace descartes_core;
  const TimingConstraint tm(dt);
  return boost::make_shared<AxialSymmetricPt>(pose, BLENDING_ANGLE_DISCRETIZATION,
                                              AxialSymmetricPt::Z_AXIS, tm);
}

/**
 * @brief Computes a 'cost' value for a robot motion between 'source' and 'target'
 * @param source  The joint configuration at start of motion
 * @param target  The joint configuration at end of motion
 * @return cost value
 */
double freeSpaceCostFunctionBlending(const std::vector<double>& source,
                                     const std::vector<double>& target)
{
  // The cost function here penalizes large single joint motions in an effort to
  // keep the robot from flipping a joint, even if some other joints have to move
  // a bit more.
  double cost = 0.0;
  for (std::size_t i = 0; i < source.size(); ++i)
  {
    double diff = std::abs(source[i] - target[i]);
    if (diff > FREE_SPACE_MAX_ANGLE_DELTA)
      cost += FREE_SPACE_ANGLE_PENALTY * diff;
    else
      cost += diff;
  }
  return cost;
}

/**
 * @brief transforms an input, in the form of a reference pose and points relative to that pose,
 * into Descartes'
 *        native format. Also adds in associated parameters.
 * @param ref The reference posed that all points are multiplied by. Should be in the world space of
 * the blend move group.
 * @param points Sequence of points (relative to ref and the world space of blending robot model)
 * @param params Surface blending parameters, including info such as traversal speed
 * @return The input trajectory encoded in Descartes points
 */
static godel_process_planning::DescartesTraj
toDescartesTraj(const geometry_msgs::Pose& ref, const std::vector<geometry_msgs::Point>& points,
                const godel_msgs::BlendingPlanParameters& params)
{
  DescartesTraj traj;
  traj.reserve(points.size());
  if (points.empty())
    return traj;

  Eigen::Affine3d last_pose = createNominalTransform(ref, points.front());

  for (std::size_t i = 0; i < points.size(); ++i)
  {
    Eigen::Affine3d this_pose = createNominalTransform(ref, points[i]);
    // O(1) jerky - may need to revisit this time parameterization later. This at least allows
    // Descartes to perform some optimizations in its graph serach.
    double dt = (this_pose.translation() - last_pose.translation()).norm() / params.traverse_spd;
    traj.push_back(toDescartesPt(this_pose, dt));
    last_pose = this_pose;
  }

  return traj;
}


static EigenSTL::vector_Affine3d linearMoveZ(const Eigen::Affine3d& origin, double step_size, int steps)
{
  EigenSTL::vector_Affine3d result (steps);

  for (int i = 0; i < steps; ++i)
  {
    result[i] = origin * Eigen::Translation3d(0, 0, step_size * (i + 1));
  }

  return result;
}

/**
 * @brief toDescartesTraj
 * @param poses
 * @param params
 * @return
 */
static godel_process_planning::DescartesTraj
toDescartesTraj(const std::vector<geometry_msgs::PoseArray>& segments,
                const godel_msgs::BlendingPlanParameters& params)
{
  std::vector<EigenSTL::vector_Affine3d> connections;

  const static double step_size = 0.02; // m

  int steps = std::round(params.safe_traverse_height / step_size);

  // Loop over every connecting edge
  for (std::size_t i = 1; i < segments.size(); ++i)
  {
    const auto& from_pose = segments[i - 1].poses.back(); // Where we come from...
    const auto& to_pose = segments[i].poses.front();      // Where we want to end up

    Eigen::Affine3d e_from, e_to;
    tf::poseMsgToEigen(from_pose, e_from);
    tf::poseMsgToEigen(to_pose, e_to);

    // Each connecting segment has a retraction from 'from_pose'
    // And an approach to 'to_pose'
    auto from = linearMoveZ(e_from, step_size, steps);
    auto to = linearMoveZ(e_to, step_size, steps);
    std::reverse(to.begin(), to.end()); // we flip the 'to' path to keep the time ordering of the path

    connections.push_back(from);
    connections.push_back(to);
  }

  DescartesTraj traj;
  Eigen::Affine3d last_pose = createNominalTransform(segments.front().poses.front());

  for (std::size_t i = 0; i < segments.size(); ++i)
  {
    // Create Descartes trajectory for the segment path
    for (std::size_t j = 0; j < segments[i].poses.size(); ++j)
    {
      Eigen::Affine3d this_pose = createNominalTransform(segments[i].poses[j]);
      // O(1) jerky - may need to revisit this time parameterization later. This at least allows
      // Descartes to perform some optimizations in its graph serach.
      double dt = (this_pose.translation() - last_pose.translation()).norm() / params.traverse_spd;
      traj.push_back(toDescartesPt(this_pose, dt));
      last_pose = this_pose;
    }

    // If we're not on the last segment, then we have connections to add
    if (i != segments.size() - 1)
    {
      const auto& depart = connections[i * 2 + 0];
      const auto& approach = connections[i * 2 + 1];

      // Create Descartes trajectory for the departure path
      for (std::size_t j = 0; j < depart.size(); ++j)
      {
        Eigen::Affine3d this_pose = createNominalTransform(depart[j]);
        // O(1) jerky - may need to revisit this time parameterization later. This at least allows
        // Descartes to perform some optimizations in its graph serach.
        double dt = (this_pose.translation() - last_pose.translation()).norm() / params.traverse_spd;
        traj.push_back(toDescartesPt(this_pose, dt));
        last_pose = this_pose;
      }

      for (std::size_t j = 0; j < approach.size(); ++j)
      {
        Eigen::Affine3d this_pose = createNominalTransform(approach[j]);
        // O(1) jerky - may need to revisit this time parameterization later. This at least allows
        // Descartes to perform some optimizations in its graph serach.
        double dt =
            j == 0 ? 0.0 : (this_pose.translation() - last_pose.translation()).norm() / params.traverse_spd;
        traj.push_back(toDescartesPt(this_pose, dt));
        last_pose = this_pose;
      }
    } // end connections
  } // end segments

  return traj;
}

/**
 * @brief Computes a joint motion plan based on input points and the blending process; this includes
 *        motion from current position to process path and back to the starting position.
 * @param req Process plan including reference pose, points, and process parameters
 * @param res Set of approach, process, and departure trajectories
 * @return True if a valid plan was generated; false otherwise
 */
bool ProcessPlanningManager::handleBlendPlanning(godel_msgs::BlendProcessPlanning::Request& req,
                                                 godel_msgs::BlendProcessPlanning::Response& res)
{
  // Enable Collision Checks
  blend_model_->setCheckCollisions(true);

  // Precondition: There must be at least one input segments
  if (req.path.segments.empty())
  {
    ROS_WARN("Planning request contained no trajectory segments. Nothing to be done.");
    return false;
  }

  // Precondition: All input segments must have at least one pose associated with them
  for (const auto& segment : req.path.segments)
  {
    if (segment.poses.empty())
    {
      ROS_WARN("Input trajectory segment contained no poses. Invalid input.");
      return false;
    }
  }


  // Transform process path from geometry msgs to descartes points
  DescartesTraj process_points = toDescartesTraj(req.path.segments, req.params);

  // Capture the current state of the robot
  std::vector<double> current_joints = getCurrentJointState(JOINT_TOPIC_NAME);

  // Compute all of the joint poses at the start of the process path
  std::vector<std::vector<double> > start_joint_poses;
  process_points.front()->getJointPoses(*blend_model_, start_joint_poses);

  if (start_joint_poses.empty())
  {
    ROS_WARN_STREAM("Blend Planning Service: Could not compute any inverse kinematic solutions for "
                    "the first point in the process path.");

    return false;
  }

  ROS_INFO_STREAM("Number of candidate poses: " << start_joint_poses.size());

  auto start_pose = godel_process_planning::pickBestStartPose(current_joints, *blend_model_, start_joint_poses, freeSpaceCostFunctionBlending);

  DescartesTraj solved_path;
  // Calculate tool pose of robot starting config so that we can go back here on the
  // return move
  Eigen::Affine3d init_pose;
  blend_model_->getFK(current_joints, init_pose);
  // Compute the nominal tool pose of the final process point
  Eigen::Affine3d process_stop_pose;
  process_points.back()->getNominalCartPose(std::vector<double>(), *blend_model_,
                                            process_stop_pose);

  // Joint interpolate from the initial robot position to 'best' starting configuration of process
  // path
  DescartesTraj to_process = createJointPath(current_joints, start_pose);
  to_process.front() =
      descartes_core::TrajectoryPtPtr(new descartes_trajectory::JointTrajectoryPt(current_joints));

  // To get a rough estimate of process path cost, add a cartesian move from the final process point
  // to the starting position again.
  DescartesTraj from_process = createLinearPath(process_stop_pose, init_pose);
  from_process.back() =
      descartes_core::TrajectoryPtPtr(new descartes_trajectory::JointTrajectoryPt(current_joints));

  // Affix the approach and depart paths calculated above with user process path
  DescartesTraj seed_path;
  seed_path.insert(seed_path.end(), to_process.begin(), to_process.end());
  seed_path.insert(seed_path.end(), process_points.begin(), process_points.end());
  seed_path.insert(seed_path.end(), from_process.begin(), from_process.end());

  // Attempt to solve the initial path (minimize joint motion over entire plan)
  if (!descartesSolve(seed_path, blend_model_, solved_path))
  {
    return false;
  }

  // Go back over and recalculate the approach and depart segments to:
  //  1. Use a joint interpolation from start to stop if collision free
  //  2. Use MoveIt (RRT-Connect) if the above fails
  // This is the only portion of the trajectory that is collision checked. Note this
  // method also converts the Descartes points into ROS trajectories.
  try
  {
    trajectory_msgs::JointTrajectory approach =
        planFreeMove(*blend_model_, blend_group_name_, moveit_model_,
                     extractJoints(*blend_model_, *solved_path[0]),
                     extractJoints(*blend_model_, *solved_path[to_process.size()]));

    trajectory_msgs::JointTrajectory depart = planFreeMove(
        *blend_model_, blend_group_name_, moveit_model_,
        extractJoints(*blend_model_, *solved_path[to_process.size() + process_points.size() - 1]),
        extractJoints(*blend_model_, *solved_path[seed_path.size() - 1]));

    // Break out the process path from the seed path and convert to ROS messages
    DescartesTraj process_part(solved_path.begin() + to_process.size(),
                               solved_path.end() - from_process.size());
    trajectory_msgs::JointTrajectory process = toROSTrajectory(process_part, *blend_model_);

    for (std::size_t i = 0; i < process.points.size(); ++i)
    {
      const auto& pt = process.points[i];
      if (!blend_model_->isValid(pt.positions))
      {
        ROS_WARN_STREAM("Position in blending path (" << i << ") invalid: Joint limit or collision detected\n");
        return false;
      }
    }


    // Fill in result trajectories
    res.plan.trajectory_process = process;
    res.plan.trajectory_approach = approach;
    res.plan.trajectory_depart = depart;

    // Fill in result header information
    const std::vector<std::string>& joint_names =
        moveit_model_->getJointModelGroup(blend_group_name_)->getActiveJointModelNames();

    godel_process_planning::fillTrajectoryHeaders(joint_names, res.plan.trajectory_approach);
    godel_process_planning::fillTrajectoryHeaders(joint_names, res.plan.trajectory_depart);
    godel_process_planning::fillTrajectoryHeaders(joint_names, res.plan.trajectory_process);

    // set the type to blend plan
    res.plan.type = godel_msgs::ProcessPlan::BLEND_TYPE;

    return true;
  }
  catch (const std::runtime_error& e)
  {
    ROS_ERROR_STREAM("Exception caught when planning blending path: " << e.what());
    return false;
  }
}

} // end namespace
