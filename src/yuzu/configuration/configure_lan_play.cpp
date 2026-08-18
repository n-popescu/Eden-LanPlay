// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtConcurrent/QtConcurrent>
#include "common/settings.h"
#include "core/internal_network/lan_play/lan_play_connection_test.h"
#include "core/internal_network/lan_play/lan_play_stack.h"
#include "ui_configure_lan_play.h"
#include "yuzu/configuration/configure_lan_play.h"

ConfigureLanPlay::ConfigureLanPlay(QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureLanPlay>()) {
    ui->setupUi(this);

    connect(ui->lan_play_enabled, &QCheckBox::toggled, this,
            &ConfigureLanPlay::UpdateLanPlayEnabled);
    connect(ui->lan_play_test, &QPushButton::clicked, this,
            &ConfigureLanPlay::TestLanPlayConnection);

    connect(&lan_play_test_watcher,
            &QFutureWatcher<Network::LanPlay::ConnectionTestResult>::finished, this, [this]() {
                const auto result = lan_play_test_watcher.result();

                ui->lan_play_test_result->setText(QString::fromStdString(result.message));
                ui->lan_play_test->setEnabled(true);
            });

    this->SetConfiguration();
}

ConfigureLanPlay::~ConfigureLanPlay() {
    // The worker holds no reference to this, but the finished handler does, so the widget must not
    // go away while a test is still running.
    lan_play_test_watcher.waitForFinished();
}

void ConfigureLanPlay::ApplyConfiguration() {
    Settings::values.lan_play_enabled = ui->lan_play_enabled->isChecked();
    Settings::values.lan_play_server = ui->lan_play_server->text().trimmed().toStdString();
    Settings::values.lan_play_virtual_ip = ui->lan_play_virtual_ip->text().trimmed().toStdString();
    Settings::values.lan_play_ldn_mitm = ui->lan_play_ldn_mitm->isChecked();

    // The multiplayer mode can be changed while a game is running: this joins or leaves the relay
    // immediately, and re-saving unchanged settings does nothing.
    Network::LanPlay::ApplySettings();
}

void ConfigureLanPlay::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureLanPlay::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigureLanPlay::SetConfiguration() {
    // LAN Play is deliberately not locked while a game runs: joining or leaving the relay takes
    // effect immediately, which is how a player recovers from a wrong relay without restarting.
    ui->lan_play_enabled->setChecked(Settings::values.lan_play_enabled.GetValue());
    ui->lan_play_server->setText(
        QString::fromStdString(Settings::values.lan_play_server.GetValue()));
    ui->lan_play_virtual_ip->setText(
        QString::fromStdString(Settings::values.lan_play_virtual_ip.GetValue()));
    ui->lan_play_ldn_mitm->setChecked(Settings::values.lan_play_ldn_mitm.GetValue());

    UpdateLanPlayEnabled(ui->lan_play_enabled->isChecked());
}

void ConfigureLanPlay::UpdateLanPlayEnabled(bool enabled) {
    ui->lan_play_server->setEnabled(enabled);
    ui->lan_play_server_label->setEnabled(enabled);
    ui->lan_play_virtual_ip->setEnabled(enabled);
    ui->lan_play_virtual_ip_label->setEnabled(enabled);
    ui->lan_play_ldn_mitm->setEnabled(enabled);

    // A test already under way owns the button; letting this re-enable it would allow a second one.
    ui->lan_play_test->setEnabled(enabled && !lan_play_test_watcher.isRunning());
}

void ConfigureLanPlay::TestLanPlayConnection() {
    if (lan_play_test_watcher.isRunning()) {
        return;
    }

    // The fields are read here rather than in the worker, because a widget may only be touched from
    // the UI thread. The test uses its own relay connection and does not disturb a running session.
    const std::string server = ui->lan_play_server->text().trimmed().toStdString();
    const std::string virtual_ip = ui->lan_play_virtual_ip->text().trimmed().toStdString();

    ui->lan_play_test->setEnabled(false);
    ui->lan_play_test_result->setText(tr("Joining the relay and listening for a few seconds..."));

    lan_play_test_watcher.setFuture(QtConcurrent::run(
        [server, virtual_ip] { return Network::LanPlay::RunConnectionTest(server, virtual_ip); }));
}
