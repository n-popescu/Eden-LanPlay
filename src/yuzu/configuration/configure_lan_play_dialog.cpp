// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_configure_lan_play_dialog.h"
#include "yuzu/configuration/configure_lan_play.h"
#include "yuzu/configuration/configure_lan_play_dialog.h"

ConfigureLanPlayDialog::ConfigureLanPlayDialog(QWidget* parent)
    : QDialog(parent), ui(std::make_unique<Ui::ConfigureLanPlayDialog>()),
      lan_play_widget(new ConfigureLanPlay(this)) {
    ui->setupUi(this);

    ui->lanPlayLayout->addWidget(lan_play_widget);

    RetranslateUI();
}

ConfigureLanPlayDialog::~ConfigureLanPlayDialog() = default;

void ConfigureLanPlayDialog::ApplyConfiguration() {
    lan_play_widget->ApplyConfiguration();
}

void ConfigureLanPlayDialog::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QDialog::changeEvent(event);
}

void ConfigureLanPlayDialog::RetranslateUI() {
    ui->retranslateUi(this);
}
