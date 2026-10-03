#pragma once
#include <QString>
class QWidget;

// Native file dialog through xdg-desktop-portal (KDE's own dialog under Plasma) without
// loading the heavy KDE platform theme. Falls back to Qt's widget dialog.
QString chooseFile(QWidget *parent, bool save, const QString &currentPath);
