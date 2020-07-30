// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "orbitdisassemblydialog.h"

#include "ui_orbitdisassemblydialog.h"

//-----------------------------------------------------------------------------
OrbitDisassemblyDialog::OrbitDisassemblyDialog(QWidget* parent)
    : QDialog(parent), ui(new Ui::OrbitDisassemblyDialog) {
  ui->setupUi(this);
}

//-----------------------------------------------------------------------------
OrbitDisassemblyDialog::~OrbitDisassemblyDialog() { delete ui; }

//-----------------------------------------------------------------------------
void OrbitDisassemblyDialog::SetText(const std::string& a_Text) {
  ui->plainTextEdit->SetText(a_Text);
  ui->plainTextEdit->moveCursor(QTextCursor::Start);
  ui->plainTextEdit->ensureCursorVisible();
}

//-----------------------------------------------------------------------------
void OrbitDisassemblyDialog::SetDisassemblyReport(
    const DisassemblyReport& report) {
  ui->plainTextEdit->SetReport(std::make_unique<DisassemblyReport>(report));
}