#include <gtest/gtest.h>

#include <type_traits>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include "minicyber/proto/autodrive.pb.h"

namespace {

using minicyber::proto::CameraFrame;
using minicyber::proto::ControlCommand;
using minicyber::proto::FusedObstacle;
using minicyber::proto::PerceptionObstacle;
using minicyber::proto::Trajectory;
using minicyber::proto::VehicleState;

static_assert(std::is_base_of<google::protobuf::Message, CameraFrame>::value,
              "autodrive messages must satisfy the public Channel boundary");
static_assert(std::is_base_of<google::protobuf::Message, VehicleState>::value,
              "autodrive messages must satisfy the public Channel boundary");
static_assert(
    std::is_base_of<google::protobuf::Message, PerceptionObstacle>::value,
    "autodrive messages must satisfy the public Channel boundary");
static_assert(std::is_base_of<google::protobuf::Message, FusedObstacle>::value,
              "autodrive messages must satisfy the public Channel boundary");
static_assert(std::is_base_of<google::protobuf::Message, Trajectory>::value,
              "autodrive messages must satisfy the public Channel boundary");
static_assert(std::is_base_of<google::protobuf::Message, ControlCommand>::value,
              "autodrive messages must satisfy the public Channel boundary");

template <typename Message>
void ExpectSourceIdentityContract(Message* message) {
  message->set_source_sequence(17);
  message->set_source_monotonic_ns(123456789);

  std::string serialized;
  ASSERT_TRUE(message->SerializeToString(&serialized));

  Message restored;
  ASSERT_TRUE(restored.ParseFromString(serialized));
  EXPECT_TRUE(restored.has_source_sequence());
  EXPECT_TRUE(restored.has_source_monotonic_ns());
  EXPECT_EQ(restored.source_sequence(), 17u);
  EXPECT_EQ(restored.source_monotonic_ns(), 123456789u);
}

void ExpectSharedSourceFields(const google::protobuf::Descriptor* descriptor) {
  ASSERT_NE(descriptor, nullptr);
  const auto* sequence = descriptor->FindFieldByNumber(1);
  ASSERT_NE(sequence, nullptr);
  EXPECT_EQ(sequence->name(), "source_sequence");
  EXPECT_EQ(sequence->type(), google::protobuf::FieldDescriptor::TYPE_UINT64);

  const auto* monotonic_ns = descriptor->FindFieldByNumber(2);
  ASSERT_NE(monotonic_ns, nullptr);
  EXPECT_EQ(monotonic_ns->name(), "source_monotonic_ns");
  EXPECT_EQ(monotonic_ns->type(),
            google::protobuf::FieldDescriptor::TYPE_UINT64);
}

TEST(AutodriveProtoTest, EveryMessageReservesFieldsOneAndTwoForSourceIdentity) {
  ExpectSharedSourceFields(CameraFrame::descriptor());
  ExpectSharedSourceFields(VehicleState::descriptor());
  ExpectSharedSourceFields(PerceptionObstacle::descriptor());
  ExpectSharedSourceFields(FusedObstacle::descriptor());
  ExpectSharedSourceFields(Trajectory::descriptor());
  ExpectSharedSourceFields(ControlCommand::descriptor());
}

TEST(AutodriveProtoTest, EveryMessagePreservesTheSourceIdentity) {
  CameraFrame camera;
  camera.set_image_data(std::string(2048, 'c'));
  camera.set_width(1920);
  camera.set_height(1080);
  ExpectSourceIdentityContract(&camera);

  VehicleState vehicle;
  vehicle.set_speed_mps(12.5);
  vehicle.set_yaw_rate_rad_s(0.2);
  vehicle.set_steering_angle_rad(-0.1);
  ExpectSourceIdentityContract(&vehicle);

  PerceptionObstacle perception;
  perception.set_obstacle_id(9);
  perception.set_distance_m(24.0);
  perception.set_lateral_offset_m(0.4);
  perception.set_confidence(0.95);
  ExpectSourceIdentityContract(&perception);

  FusedObstacle fused;
  fused.set_obstacle_id(9);
  fused.set_distance_m(23.5);
  fused.set_lateral_offset_m(0.3);
  fused.set_relative_speed_mps(-1.2);
  ExpectSourceIdentityContract(&fused);

  Trajectory trajectory;
  trajectory.set_target_speed_mps(9.0);
  trajectory.set_target_curvature(0.02);
  trajectory.set_target_distance_m(20.0);
  ExpectSourceIdentityContract(&trajectory);

  ControlCommand command;
  command.set_throttle(0.3);
  command.set_brake(0.0);
  command.set_steering_target(0.02);
  ExpectSourceIdentityContract(&command);
}

TEST(AutodriveProtoTest, SequenceZeroRemainsAnExplicitWarmupMarker) {
  VehicleState warmup;
  warmup.set_source_sequence(0);
  warmup.set_source_monotonic_ns(1);

  EXPECT_TRUE(warmup.has_source_sequence());
  EXPECT_EQ(warmup.source_sequence(), 0u);
}

}  // namespace
