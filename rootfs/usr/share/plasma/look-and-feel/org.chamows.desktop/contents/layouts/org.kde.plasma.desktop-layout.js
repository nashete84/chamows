// Chamows default desktop: Windows 11 style taskbar at the bottom with the
// Start button and app icons centered, tray and clock on the right.

var panel = new Panel;
panel.location = "bottom";
panel.height = 2 * Math.floor(gridUnit * 2.6 / 2);
panel.hiding = "none";

panel.addWidget("org.kde.plasma.panelspacer");

var start = panel.addWidget("org.kde.plasma.kickoff");
start.currentConfigGroup = ["General"];
start.writeConfig("icon", "chamows-start");

var tasks = panel.addWidget("org.kde.plasma.icontasks");
tasks.currentConfigGroup = ["General"];
tasks.writeConfig("launchers", [
    "applications:org.kde.dolphin.desktop",
    "applications:firefox.desktop",
    "applications:chamows-store.desktop"
]);
// Keep the icons together so the spacers can center them.
tasks.writeConfig("fill", false);

panel.addWidget("org.kde.plasma.panelspacer");
panel.addWidget("org.kde.plasma.systemtray");

var clock = panel.addWidget("org.kde.plasma.digitalclock");
clock.currentConfigGroup = ["Appearance"];
clock.writeConfig("showDate", true);
clock.writeConfig("dateFormat", "shortDate");

panel.addWidget("org.kde.plasma.showdesktop");

var allDesktops = desktops();
for (var i = 0; i < allDesktops.length; i++) {
    var d = allDesktops[i];
    d.wallpaperPlugin = "org.kde.image";
    d.currentConfigGroup = ["Wallpaper", "org.kde.image", "General"];
    d.writeConfig("Image", "file:///usr/share/chamows/wallpaper.png");
}
