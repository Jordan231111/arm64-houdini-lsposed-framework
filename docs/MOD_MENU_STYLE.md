# Mod menu style

The template menu mirrors the existing Nyx-style panel:

- dark translucent panel: `#E00E0E14`
- lavender border/accent: `#B89AFF` and `#6B5BFF`
- rounded rectangle radius: `16dp`
- dark pill buttons: `#1A1A24` with `#3A3550` stroke
- small oval bubble with `Nyx` text
- flat buttons with no default Material elevation

Behavior:

- bubble is draggable
- tap bubble opens/closes the rectangular panel
- rectangular panel is draggable when opened by dragging the title/subtitle header
- overlay is added as a child of the target Activity decor view rather than as a top-level `TYPE_APPLICATION_OVERLAY` window, so it does not rely on `SYSTEM_ALERT_WINDOW` in the target process

Main implementation:

- `app/src/main/java/com/template/lsposed/ui/OverlayController.java`
