// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gmock/gmock-matchers.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/channel_arguments.h>
#include <gtest/gtest.h>

#include <QApplication>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QTableView>
#include <QTest>
#include <chrono>
#include <memory>
#include <thread>

#include "OrbitBase/ExecutablePath.h"
#include "OrbitBase/Result.h"
#include "OrbitBase/TestUtils.h"
#include "OrbitGgp/Instance.h"
#include "OrbitSsh/Context.h"
#include "SessionSetup/ConnectToStadiaWidget.h"
#include "SessionSetup/Connections.h"
#include "SessionSetup/DeploymentConfigurations.h"
#include "SessionSetup/OverlayWidget.h"
#include "SessionSetup/ServiceDeployManager.h"

namespace orbit_session_setup {

using orbit_base::HasError;
using orbit_base::HasValue;

namespace {

class ConnectToStadiaWidgetTestFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    auto ssh_context = orbit_ssh::Context::Create();
    ASSERT_TRUE(ssh_context.has_value());
    ssh_context_ = std::make_unique<orbit_ssh::Context>(std::move(ssh_context.value()));
    ssh_artifacts_ = std::make_unique<SshConnectionArtifacts>(
        ssh_context_.get(), ServiceDeployManager::GrpcPort{0}, &deploy_config_);

    const std::filesystem::path mock_ggp_working =
        orbit_base::GetExecutableDir() / "OrbitMockGgpWorking";
    widget_ = std::make_unique<ConnectToStadiaWidget>(QString::fromStdString(mock_ggp_working));

    widget_->SetSshConnectionArtifacts(ssh_artifacts_.get());

    widget_->show();

    overlay_ = widget_->findChild<OverlayWidget*>("instancesTableOverlay");
    ASSERT_NE(overlay_, nullptr);

    refresh_button_ = widget_->findChild<QPushButton*>("refreshButton");
    ASSERT_NE(refresh_button_, nullptr);

    instances_table_view_ = widget_->findChild<QTableView*>("instancesTableView");
    ASSERT_NE(instances_table_view_, nullptr);
  }

  std::unique_ptr<orbit_ssh::Context> ssh_context_;
  DeploymentConfiguration deploy_config_{NoDeployment{}};
  std::unique_ptr<SshConnectionArtifacts> ssh_artifacts_;
  std::unique_ptr<ConnectToStadiaWidget> widget_;

  OverlayWidget* overlay_ = nullptr;
  QPushButton* refresh_button_ = nullptr;
  QTableView* instances_table_view_ = nullptr;
};

void WaitForMockGgp() {
  // OrbitMockGgpWorking has a built in delay of 50 milliseconds, hence a call will take at least
  // that long. An additional 50 milliseconds are waited here to allow spawning and clean up of the
  // process.
  std::this_thread::sleep_for(std::chrono::milliseconds{50 + 50});
}

}  // namespace

TEST(ConnectToStadiaWidget, IsSetActive) {
  ConnectToStadiaWidget widget{};

  // This is the radio button of the ConnectToStadiaWidget, which is one of three (with local
  // profiling enabled) in SessionSetupDialog. This radio button should always be enabled, even when
  // the widget is inactive. This has to be the case so the user can re-activate the widget.
  auto* radio_button = widget.findChild<QRadioButton*>("radioButton");
  ASSERT_NE(radio_button, nullptr);

  widget.show();

  // default
  EXPECT_TRUE(widget.IsActive());
  EXPECT_TRUE(radio_button->isEnabled());

  // set active false
  widget.SetActive(false);
  EXPECT_FALSE(widget.IsActive());
  EXPECT_TRUE(radio_button->isEnabled());

  // set active true
  widget.SetActive(true);
  EXPECT_TRUE(widget.IsActive());
  EXPECT_TRUE(radio_button->isEnabled());
}

TEST(ConnectToStadiaWidget, CallStartAndFail) {
  {
    const std::filesystem::path mock_ggp_working =
        orbit_base::GetExecutableDir() / "OrbitMockGgpWorking";
    ConnectToStadiaWidget widget{QString::fromStdString(mock_ggp_working)};
    ErrorMessageOr<void> start_result = widget.Start();
    std::string expected_error{
        "Internal error: Unable to start ConnectToStadiaWidget, ssh_connection_artifacts_ is not "
        "set."};
    EXPECT_THAT(start_result, HasError(expected_error));
    EXPECT_FALSE(widget.isEnabled());
    auto* radio_button = widget.findChild<QRadioButton*>("radioButton");
    ASSERT_NE(radio_button, nullptr);
    EXPECT_THAT(radio_button->toolTip().toStdString(), expected_error);
  }

  {
    ConnectToStadiaWidget widget{"non/existing/path/to/ggp"};

    auto ssh_context = orbit_ssh::Context::Create();
    ASSERT_TRUE(ssh_context.has_value());
    DeploymentConfiguration deploy_config{NoDeployment{}};
    SshConnectionArtifacts ssh_artifacts{&ssh_context.value(), ServiceDeployManager::GrpcPort{0},
                                         &deploy_config};

    widget.SetSshConnectionArtifacts(&ssh_artifacts);
    ErrorMessageOr<void> start_result = widget.Start();
    std::string error_substring{"Unable to use ggp cli"};
    EXPECT_THAT(start_result, HasError(error_substring));
    EXPECT_FALSE(widget.isEnabled());

    auto* radio_button = widget.findChild<QRadioButton*>("radioButton");
    ASSERT_NE(radio_button, nullptr);
    EXPECT_THAT(radio_button->toolTip().toStdString(), testing::HasSubstr(error_substring));
  }
}

TEST_F(ConnectToStadiaWidgetTestFixture,
       StartWithoutPriorConnectionAndLoadInstancesAndReloadInstances) {
  ErrorMessageOr<void> start_result = widget_->Start();
  EXPECT_THAT(start_result, HasValue());

  // After start (and after processing state machine transitions), the instances are loaded
  // automatically. This means the overlay is visible, the refresh button is disabled and the
  // instance list is empty;
  QApplication::processEvents();
  EXPECT_TRUE(overlay_->isVisible());
  EXPECT_FALSE(refresh_button_->isEnabled());
  EXPECT_EQ(instances_table_view_->model()->rowCount(), 0);

  WaitForMockGgp();
  QApplication::processEvents();

  EXPECT_FALSE(overlay_->isVisible());
  EXPECT_TRUE(refresh_button_->isEnabled());
  EXPECT_TRUE(instances_table_view_->isEnabled());
  // OrbitMockGgpWorking returns 2 mock instances.
  EXPECT_EQ(instances_table_view_->model()->rowCount(), 2);

  // Reload instances
  QTest::mouseClick(refresh_button_, Qt::MouseButton::LeftButton);
  QApplication::processEvents();
  EXPECT_TRUE(overlay_->isVisible());
  EXPECT_FALSE(refresh_button_->isEnabled());
  EXPECT_EQ(instances_table_view_->model()->rowCount(), 0);

  // Wait until reloading is done
  WaitForMockGgp();
  QApplication::processEvents();
  EXPECT_FALSE(overlay_->isVisible());
  EXPECT_TRUE(refresh_button_->isEnabled());
  EXPECT_TRUE(instances_table_view_->isEnabled());
  EXPECT_EQ(instances_table_view_->model()->rowCount(), 2);
}

TEST_F(ConnectToStadiaWidgetTestFixture, StartWithExistingConnection) {
  std::shared_ptr<grpc::Channel> grpc_channel = grpc::CreateCustomChannel(
      "127.0.0.1:0", grpc::InsecureChannelCredentials(), grpc::ChannelArguments());
  StadiaConnection connection{orbit_ggp::Instance{},
                              std::make_unique<ServiceDeployManager>(
                                  &deploy_config_, ssh_context_.get(), orbit_ssh::Credentials{},
                                  ServiceDeployManager::GrpcPort{0}),
                              std::move(grpc_channel)};

  widget_->SetConnection(std::move(connection));
  auto result = widget_->Start();
  EXPECT_THAT(result, HasValue());

  std::optional<StadiaConnection> clear_result = widget_->StopAndClearConnection();
  EXPECT_TRUE(clear_result.has_value());
}

TEST_F(ConnectToStadiaWidgetTestFixture, SelectRememberedInstance) {
  QCoreApplication::setOrganizationName("The Orbit Authors");
  QCoreApplication::setApplicationName("ConnectToStadiaWidgetTest");
  QSettings settings;
  settings.setValue("RememberChosenInstance", "id/of/instance2");
  SetUp();

  auto start_result = widget_->Start();
  ASSERT_THAT(start_result, HasValue());

  QApplication::processEvents();
  WaitForMockGgp();
  QApplication::processEvents();
  EXPECT_TRUE(instances_table_view_->selectionModel()->hasSelection());
  ASSERT_TRUE(instances_table_view_->selectionModel()
                  ->currentIndex()
                  .data(Qt::UserRole)
                  .canConvert<orbit_ggp::Instance>());
  EXPECT_EQ(instances_table_view_->selectionModel()
                ->currentIndex()
                .data(Qt::UserRole)
                .value<orbit_ggp::Instance>()
                .id,
            "id/of/instance2");
}

}  // namespace orbit_session_setup