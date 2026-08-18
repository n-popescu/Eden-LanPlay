// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QtConcurrent/QtConcurrent>
#include "common/settings.h"
#include "core/core.h"
#include "core/internal_network/lan_play/lan_play_stack.h"
#include "core/internal_network/network_interface.h"
#include "ui_configure_network.h"
#include "yuzu/configuration/configure_network.h"

ConfigureNetwork::ConfigureNetwork(const Core::System& system_, QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureNetwork>()), system{system_} {
    ui->setupUi(this);
    for (const auto& iface : Network::GetAvailableNetworkInterfaces())
        ui->network_interface->addItem(QString::fromStdString(iface.name));

    connect(ui->lan_play_enabled, &QCheckBox::toggled, this, &ConfigureNetwork::UpdateLanPlayEnabled);

    this->SetConfiguration();
}

ConfigureNetwork::~ConfigureNetwork() = default;

void ConfigureNetwork::ApplyConfiguration() {
    Settings::values.network_interface = ui->network_interface->currentText().toStdString();
    Settings::values.airplane_mode = ui->airplane_mode->isChecked();

    Settings::values.lan_play_enabled = ui->lan_play_enabled->isChecked();
    Settings::values.lan_play_server = ui->lan_play_server->text().trimmed().toStdString();
    Settings::values.lan_play_virtual_ip = ui->lan_play_virtual_ip->text().trimmed().toStdString();

    // The multiplayer mode can be changed while a game is running: this joins or leaves the relay
    // immediately, and re-saving unchanged settings does nothing.
    Network::LanPlay::ApplySettings();
}

void ConfigureNetwork::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureNetwork::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigureNetwork::SetConfiguration() {
    const bool runtime_lock = !system.IsPoweredOn();
    auto const network_interface = Settings::values.network_interface.GetValue();
    auto const airplane_mode = Settings::values.airplane_mode.GetValue();
    ui->network_interface->setCurrentText(QString::fromStdString(network_interface));
    ui->network_interface->setEnabled(runtime_lock);
    ui->airplane_mode->setChecked(airplane_mode);

    // LAN Play is deliberately not locked while a game runs: joining or leaving the relay takes
    // effect immediately, which is how a player recovers from a wrong relay without restarting.
    ui->lan_play_enabled->setChecked(Settings::values.lan_play_enabled.GetValue());
    ui->lan_play_server->setText(
        QString::fromStdString(Settings::values.lan_play_server.GetValue()));
    ui->lan_play_virtual_ip->setText(
        QString::fromStdString(Settings::values.lan_play_virtual_ip.GetValue()));

    UpdateLanPlayEnabled(ui->lan_play_enabled->isChecked());
}

void ConfigureNetwork::UpdateLanPlayEnabled(bool enabled) {
    ui->lan_play_server->setEnabled(enabled);
    ui->lan_play_server_label->setEnabled(enabled);
    ui->lan_play_virtual_ip->setEnabled(enabled);
    ui->lan_play_virtual_ip_label->setEnabled(enabled);
}
