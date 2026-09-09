#ifndef QT_GAME_OS_TYPE_H
#define QT_GAME_OS_TYPE_H

#include <QString>
#include <QStringList>

enum class GameOsType {
    Unknown,
    Windows,
    Win32,
    MacOSX,
    MacOs,
    Psp,
    Ios,
    Android,
    Symbian,
    Linux,
    WinPhone,
    Tizen,
    Win8Native,
    WiiU,
    ThreeDs,
    PsVita,
    Bb10,
    Ps4,
    XboxOne,
    Ps3,
    Xbox360,
    Uwp,
    Amazon,
    Switch
};

QString gameOsTypeToCanonicalName(GameOsType value);
GameOsType gameOsTypeFromCanonicalName(const QString& canonicalName);
QString gameOsTypeLabel(GameOsType value);
QStringList allGameOsTypeNames();
QString prettyGameOsTypeName(const QString& canonicalName);
QString canonicalGameOsTypeName(const QString& displayName);
QString defaultGameOsType(const QString& path);

#endif // QT_GAME_OS_TYPE_H
