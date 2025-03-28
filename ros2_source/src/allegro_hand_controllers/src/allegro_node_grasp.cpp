#include "allegro_node_grasp.h"

#include "bhand/BHand.h"

// The only topic specific to the 'grasp' controller is the envelop torque.
const std::string ENVELOP_TORQUE_TOPIC = "allegroHand/envelop_torque";

// Define a map from string (received message) to eMotionType (Bhand controller grasp).
std::map<std::string, eMotionType> bhand_grasps = {
        {"home",     eMotionType_HOME},
        {"ready",    eMotionType_READY},  // ready position
        {"grasp_3",  eMotionType_GRASP_3},  // grasp with 3 fingers
        {"grasp_4",  eMotionType_GRASP_4},  // grasp with 4 fingers
        {"pinch_it", eMotionType_PINCH_IT},  // pinch, index & thumb
        {"pinch_mt", eMotionType_PINCH_MT},  // pinch, middle & thumb
        {"envelop",  eMotionType_ENVELOP},  // envelop grasp (power-y)
        {"off",      eMotionType_NONE},  // turn joints off
        {"gravcomp", eMotionType_GRAVITY_COMP},  // gravity compensation
        // These ones do not appear to do anything useful (or anything at all):
        // {"pregrasp", eMotionType_PRE_SHAPE},  // not sure what this is supposed to do.
        // {"move_object", eMotionType_OBJECT_MOVING},
        // {"move_fingertip", eMotionType_FINGERTIP_MOVING}
};

AllegroNodeGrasp::AllegroNodeGrasp(const std::string nodeName)
    : AllegroNode(nodeName)
{

  initController(whichHand);


  joint_cmd_sub = this->create_subscription<sensor_msgs::msg::JointState>(
      DESIRED_STATE_TOPIC, 3, std::bind(&AllegroNodeGrasp::setJointCallback, this, std::placeholders::_1));
  lib_cmd_sub = this->create_subscription<std_msgs::msg::String>(
      LIB_CMD_TOPIC, 1, std::bind(&AllegroNodeGrasp::libCmdCallback, this, std::placeholders::_1));
  envelop_torque_sub = this->create_subscription<std_msgs::msg::Float32>(
      ENVELOP_TORQUE_TOPIC, 1, std::bind(&AllegroNodeGrasp::envelopTorqueCallback, this, std::placeholders::_1));
}

AllegroNodeGrasp::~AllegroNodeGrasp() {
  delete pBHand;
}

void AllegroNodeGrasp::libCmdCallback(const std_msgs::msg::String &msg)
{
  RCLCPP_INFO(this->get_logger(), "CTRL: Heard: [%s]", msg.data.c_str());
  const std::string lib_cmd = msg.data;

  // Main behavior: apply the grasp directly from the map. Secondary behaviors can still be handled
  // normally (case-by-case basis), note these should *not* be in the map.
  auto itr = bhand_grasps.find(msg.data);
  if (itr != bhand_grasps.end()) {
    pBHand->SetMotionType(itr->second);
    RCLCPP_INFO(this->get_logger(), "motion type = %d", itr->second);
  } else if (lib_cmd.compare("pdControl") == 0) {
    // Desired position only necessary if in PD Control mode
    pBHand->SetJointDesiredPosition(desired_position);
    pBHand->SetMotionType(eMotionType_JOINT_PD);
  } else if (lib_cmd.compare("save") == 0) {
    for (int i = 0; i < DOF_JOINTS; i++)
      desired_position[i] = current_position[i];
  } else {
    RCLCPP_WARN(this->get_logger(), "Unknown commanded grasp: %s.", lib_cmd.c_str());
  }
}

// Called when a desired joint position message is received
void AllegroNodeGrasp::setJointCallback(const sensor_msgs::msg::JointState &msg)
{
  mutex->lock();

  for (int i = 0; i < DOF_JOINTS; i++)
    desired_position[i] = msg.position[i];
  mutex->unlock();

  pBHand->SetJointDesiredPosition(desired_position);
  pBHand->SetMotionType(eMotionType_JOINT_PD);
}

// The grasp controller can set the desired envelop grasp torque by listening to
// Float32 messages on ENVELOP_TORQUE_TOPIC ("allegroHand/envelop_torque").
void AllegroNodeGrasp::envelopTorqueCallback(const std_msgs::msg::Float32 &msg)
{
  const double torque = msg.data;
  RCLCPP_INFO(this->get_logger(), "Setting envelop torque to %.3f.", torque);
  pBHand->SetEnvelopTorqueScalar(torque);
}

void AllegroNodeGrasp::computeDesiredTorque() {
  // compute control torque using Bhand library
  pBHand->SetJointPosition(current_position_filtered);

  // BHand lib control updated with time stamp
  pBHand->UpdateControl((double) frame * ALLEGRO_CONTROL_TIME_INTERVAL);

  // Necessary torque obtained from Bhand lib
  pBHand->GetJointTorque(desired_torque);

  //ROS_INFO("desired torque = %.3f %.3f %.3f %.3f", desired_torque[0], desired_torque[1], desired_torque[2], desired_torque[3]);
}

void AllegroNodeGrasp::initController(const std::string &whichHand)
{
  // Initialize BHand controller
  if (whichHand.compare("left") == 0) {
    pBHand = new BHand(eHandType_Left);
    RCLCPP_WARN(this->get_logger(), "CTRL: Left Allegro Hand controller initialized.");
  }
  else {
    pBHand = new BHand(eHandType_Right);
    RCLCPP_WARN(this->get_logger(), "CTRL: Right Allegro Hand controller initialized.");
  }
  pBHand->SetTimeInterval(ALLEGRO_CONTROL_TIME_INTERVAL);
  pBHand->SetMotionType(eMotionType_NONE);

  // sets initial desired pos at start pos for PD control
  for (int i = 0; i < DOF_JOINTS; i++)
    desired_position[i] = current_position[i];

  RCLCPP_INFO(this->get_logger(), "*************************************");
  RCLCPP_INFO(this->get_logger(), "         Grasp (BHand) Method        ");
  RCLCPP_INFO(this->get_logger(), "-------------------------------------");
  RCLCPP_INFO(this->get_logger(), "         Every command works.        ");
  RCLCPP_INFO(this->get_logger(), "*************************************");
}

void AllegroNodeGrasp::doIt(bool polling) {
  auto this_node = std::shared_ptr<AllegroNodeGrasp>(this);
  int control_cycle = (int)(1 / ALLEGRO_CONTROL_TIME_INTERVAL);
  rclcpp::Rate rate(control_cycle);
  if (polling) {
    RCLCPP_INFO(this->get_logger(), " Polling = true.");
    while (rclcpp::ok())
    {
      updateController();
      rclcpp::spin_some(this_node);
      rate.sleep();
    }
  } else {
    RCLCPP_INFO(this->get_logger(), "Polling = false.");

    // Timer callback (not recommended).
    rclcpp::TimerBase::SharedPtr timer = startTimerCallback();
    rclcpp::spin(this_node);
  }
}

int main(int argc, char *argv[])
{
  rclcpp::init(argc, argv);

  bool polling = false;
  if (argv[1] == std::string("true")) {
    polling = true;
  }

  RCLCPP_INFO(rclcpp::get_logger("allgro_node_grasp"), "Start controller with polling = %d", polling);
  AllegroNodeGrasp allegroNode("allegro_node_grasp");
  allegroNode.doIt(polling);
}
