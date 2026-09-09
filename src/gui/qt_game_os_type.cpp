#include "qt_game_os_type.h"

#include <QFileInfo>
#include <QString>

namespace {

struct GameOsTypeName {
    GameOsType value;
    const char* label;
};

const GameOsTypeName kGameOsTypeNames[] = {
    {GameOsType::Unknown, "Unknown"},
    {GameOsType::Windows, "Windows"},
    {GameOsType::Win32, "Win32"},
    {GameOsType::MacOSX, "Mac OSX"},
    {GameOsType::MacOs, "macOS"},
    {GameOsType::Psp, "PSP"},
    {GameOsType::Ios, "iOS"},
    {GameOsType::Android, "Android"},
    {GameOsType::Symbian, "Symbian"},
    {GameOsType::Linux, "Linux"},
    {GameOsType::WinPhone, "Windows Phone"},
    {GameOsType::Tizen, "Tizen"},
    {GameOsType::Win8Native, "Windows 8 Native"},
    {GameOsType::WiiU, "Wii U"},
    {GameOsType::ThreeDs, "3DS"},
    {GameOsType::PsVita, "PS Vita"},
    {GameOsType::Bb10, "BlackBerry 10"},
    {GameOsType::Ps4, "PS4"},
    {GameOsType::XboxOne, "Xbox One"},
    {GameOsType::Ps3, "PS3"},
    {GameOsType::Xbox360, "Xbox 360"},
    {GameOsType::Uwp, "UWP"},
    {GameOsType::Amazon, "Amazon"},
    {GameOsType::Switch, "Switch"},
};

} // namespace

QString gameOsTypeToCanonicalName(GameOsType value) {
    switch (value) {
        case GameOsType::Unknown:
            return QStringLiteral("unknown");
        case GameOsType::Windows:
            return QStringLiteral("windows");
        case GameOsType::Win32:
            return QStringLiteral("win32");
        case GameOsType::MacOSX:
            return QStringLiteral("macosx");
        case GameOsType::MacOs:
            return QStringLiteral("macos");
        case GameOsType::Psp:
            return QStringLiteral("psp");
        case GameOsType::Ios:
            return QStringLiteral("ios");
        case GameOsType::Android:
            return QStringLiteral("android");
        case GameOsType::Symbian:
            return QStringLiteral("symbian");
        case GameOsType::Linux:
            return QStringLiteral("linux");
        case GameOsType::WinPhone:
            return QStringLiteral("winphone");
        case GameOsType::Tizen:
            return QStringLiteral("tizen");
        case GameOsType::Win8Native:
            return QStringLiteral("win8native");
        case GameOsType::WiiU:
            return QStringLiteral("wiiu");
        case GameOsType::ThreeDs:
            return QStringLiteral("3ds");
        case GameOsType::PsVita:
            return QStringLiteral("psvita");
        case GameOsType::Bb10:
            return QStringLiteral("bb10");
        case GameOsType::Ps4:
            return QStringLiteral("ps4");
        case GameOsType::XboxOne:
            return QStringLiteral("xboxone");
        case GameOsType::Ps3:
            return QStringLiteral("ps3");
        case GameOsType::Xbox360:
            return QStringLiteral("xbox360");
        case GameOsType::Uwp:
            return QStringLiteral("uwp");
        case GameOsType::Amazon:
            return QStringLiteral("amazon");
        case GameOsType::Switch:
            return QStringLiteral("switch");
    }
    return QStringLiteral("unknown");
}

GameOsType gameOsTypeFromCanonicalName(const QString& canonicalName) {
    const QString normalized = canonicalName.trimmed().toLower();
    if (normalized == QStringLiteral("unknown")) {
        return GameOsType::Unknown;
    }
    if (normalized == QStringLiteral("windows")) {
        return GameOsType::Windows;
    }
    if (normalized == QStringLiteral("win32")) {
        return GameOsType::Win32;
    }
    if (normalized == QStringLiteral("macosx")) {
        return GameOsType::MacOSX;
    }
    if (normalized == QStringLiteral("macos")) {
        return GameOsType::MacOs;
    }
    if (normalized == QStringLiteral("psp")) {
        return GameOsType::Psp;
    }
    if (normalized == QStringLiteral("ios")) {
        return GameOsType::Ios;
    }
    if (normalized == QStringLiteral("android")) {
        return GameOsType::Android;
    }
    if (normalized == QStringLiteral("symbian")) {
        return GameOsType::Symbian;
    }
    if (normalized == QStringLiteral("linux")) {
        return GameOsType::Linux;
    }
    if (normalized == QStringLiteral("winphone")) {
        return GameOsType::WinPhone;
    }
    if (normalized == QStringLiteral("tizen")) {
        return GameOsType::Tizen;
    }
    if (normalized == QStringLiteral("win8native")) {
        return GameOsType::Win8Native;
    }
    if (normalized == QStringLiteral("wiiu")) {
        return GameOsType::WiiU;
    }
    if (normalized == QStringLiteral("3ds")) {
        return GameOsType::ThreeDs;
    }
    if (normalized == QStringLiteral("psvita")) {
        return GameOsType::PsVita;
    }
    if (normalized == QStringLiteral("bb10")) {
        return GameOsType::Bb10;
    }
    if (normalized == QStringLiteral("ps4")) {
        return GameOsType::Ps4;
    }
    if (normalized == QStringLiteral("xboxone")) {
        return GameOsType::XboxOne;
    }
    if (normalized == QStringLiteral("ps3")) {
        return GameOsType::Ps3;
    }
    if (normalized == QStringLiteral("xbox360")) {
        return GameOsType::Xbox360;
    }
    if (normalized == QStringLiteral("uwp")) {
        return GameOsType::Uwp;
    }
    if (normalized == QStringLiteral("amazon")) {
        return GameOsType::Amazon;
    }
    if (normalized == QStringLiteral("switch")) {
        return GameOsType::Switch;
    }
    return GameOsType::Unknown;
}

QString gameOsTypeLabel(GameOsType value) {
    switch (value) {
        case GameOsType::Unknown:
            return QStringLiteral("Unknown");
        case GameOsType::Windows:
            return QStringLiteral("Windows");
        case GameOsType::Win32:
            return QStringLiteral("Win32");
        case GameOsType::MacOSX:
            return QStringLiteral("Mac OSX");
        case GameOsType::MacOs:
            return QStringLiteral("macOS");
        case GameOsType::Psp:
            return QStringLiteral("PSP");
        case GameOsType::Ios:
            return QStringLiteral("iOS");
        case GameOsType::Android:
            return QStringLiteral("Android");
        case GameOsType::Symbian:
            return QStringLiteral("Symbian");
        case GameOsType::Linux:
            return QStringLiteral("Linux");
        case GameOsType::WinPhone:
            return QStringLiteral("Windows Phone");
        case GameOsType::Tizen:
            return QStringLiteral("Tizen");
        case GameOsType::Win8Native:
            return QStringLiteral("Windows 8 Native");
        case GameOsType::WiiU:
            return QStringLiteral("Wii U");
        case GameOsType::ThreeDs:
            return QStringLiteral("3DS");
        case GameOsType::PsVita:
            return QStringLiteral("PS Vita");
        case GameOsType::Bb10:
            return QStringLiteral("BlackBerry 10");
        case GameOsType::Ps4:
            return QStringLiteral("PS4");
        case GameOsType::XboxOne:
            return QStringLiteral("Xbox One");
        case GameOsType::Ps3:
            return QStringLiteral("PS3");
        case GameOsType::Xbox360:
            return QStringLiteral("Xbox 360");
        case GameOsType::Uwp:
            return QStringLiteral("UWP");
        case GameOsType::Amazon:
            return QStringLiteral("Amazon");
        case GameOsType::Switch:
            return QStringLiteral("Switch");
    }
    return QStringLiteral("Unknown");
}

QStringList allGameOsTypeNames() {
    QStringList names;
    names.reserve(static_cast<int>(std::size(kGameOsTypeNames)));
    for (const GameOsTypeName& osType : kGameOsTypeNames) {
        names << QString::fromLatin1(osType.label);
    }
    return names;
}

QString prettyGameOsTypeName(const QString& canonicalName) {
    const GameOsType value = gameOsTypeFromCanonicalName(canonicalName);
    return gameOsTypeLabel(value);
}

QString canonicalGameOsTypeName(const QString& displayName) {
    const QString normalized = displayName.trimmed().toLower();
    for (const GameOsTypeName& osType : kGameOsTypeNames) {
        const QString label = QString::fromLatin1(osType.label).toLower();
        if (label == normalized) {
            return gameOsTypeToCanonicalName(osType.value);
        }
    }
    return gameOsTypeToCanonicalName(gameOsTypeFromCanonicalName(normalized));
}

QString defaultGameOsType(const QString& path) {
    const QString suffix = QFileInfo(path).completeSuffix();
    if (suffix.compare(QStringLiteral("ios"), Qt::CaseInsensitive) == 0) {
        return gameOsTypeToCanonicalName(GameOsType::MacOs);
    }
    return gameOsTypeToCanonicalName(GameOsType::Windows);
}
