// Native application menu (File / Edit / View), built from the frontend via
// @tauri-apps/api/menu so each item's action calls straight into the React
// project handlers. Built once on mount; actions are read through a ref so they
// always invoke the latest handlers without rebuilding the menu.

import { useEffect, useRef } from "react";
import { Menu, MenuItem, PredefinedMenuItem, Submenu } from "@tauri-apps/api/menu";
import { isTauriContext } from "./tauri";

export interface MenuActions {
  openNewWindow: () => void;
  openProject: () => void;
  save: () => void;
  saveAs: () => void;
  openPreferences: () => void;
  quit: () => void;
  toggleHistory: () => void;
}

/**
 * Keyboard shortcuts for the File menu and the Run history toggle, handled in
 * the frontend because the WebView swallows the native menu accelerators on
 * Windows/Linux (browser-reserved keys: Ctrl+N/O/S/H/…). preventDefault stops
 * the browser action (e.g. Ctrl+H = history) and we run the menu action
 * instead. On macOS the native menu honours its own key equivalents, so we skip
 * the JS handler there to avoid double-firing.
 */
export function useMenuShortcuts(actions: MenuActions) {
  const ref = useRef(actions);
  ref.current = actions;
  useEffect(() => {
    const isMac = typeof navigator !== "undefined" && /Mac/i.test(navigator.platform || navigator.userAgent);
    if (isMac) return;
    const onKey = (e: KeyboardEvent) => {
      if (!e.ctrlKey || e.metaKey || e.altKey) return;
      const k = e.key.toLowerCase();
      const a = ref.current;
      if (k === "n" && !e.shiftKey) { e.preventDefault(); a.openNewWindow(); }
      else if (k === "o" && !e.shiftKey) { e.preventDefault(); a.openProject(); }
      else if (k === "s") { e.preventDefault(); if (e.shiftKey) a.saveAs(); else a.save(); }
      else if (k === "q") { e.preventDefault(); a.quit(); }
      else if (k === "," ) { e.preventDefault(); a.openPreferences(); }
      else if (k === "h" && !e.shiftKey) { e.preventDefault(); a.toggleHistory(); }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, []);
}

export function useAppMenu(actions: MenuActions) {
  const ref = useRef(actions);
  ref.current = actions;

  useEffect(() => {
    if (!isTauriContext()) return;
    let disposed = false;
    (async () => {
      const fileSub = await Submenu.new({
        text: "File",
        items: [
          await MenuItem.new({ id: "newWindow", text: "New Window", accelerator: "CmdOrCtrl+N", action: () => ref.current.openNewWindow() }),
          await MenuItem.new({ id: "open", text: "Open…", accelerator: "CmdOrCtrl+O", action: () => ref.current.openProject() }),
          await MenuItem.new({ id: "save", text: "Save", accelerator: "CmdOrCtrl+S", action: () => ref.current.save() }),
          await MenuItem.new({ id: "saveAs", text: "Save As…", accelerator: "CmdOrCtrl+Shift+S", action: () => ref.current.saveAs() }),
          await PredefinedMenuItem.new({ item: "Separator" }),
          await MenuItem.new({ id: "preferences", text: "Preferences…", accelerator: "CmdOrCtrl+,", action: () => ref.current.openPreferences() }),
          await PredefinedMenuItem.new({ item: "Separator" }),
          await MenuItem.new({ id: "quit", text: "Quit", accelerator: "CmdOrCtrl+Q", action: () => ref.current.quit() }),
        ],
      });
      const editSub = await Submenu.new({
        text: "Edit",
        items: [
          await PredefinedMenuItem.new({ item: "Undo" }),
          await PredefinedMenuItem.new({ item: "Redo" }),
          await PredefinedMenuItem.new({ item: "Separator" }),
          await PredefinedMenuItem.new({ item: "Cut" }),
          await PredefinedMenuItem.new({ item: "Copy" }),
          await PredefinedMenuItem.new({ item: "Paste" }),
          await PredefinedMenuItem.new({ item: "SelectAll" }),
        ],
      });
      const viewSub = await Submenu.new({
        text: "View",
        items: [
          await MenuItem.new({ id: "history", text: "Run history", accelerator: "CmdOrCtrl+H", action: () => ref.current.toggleHistory() }),
        ],
      });
      const menu = await Menu.new({ items: [fileSub, editSub, viewSub] });
      if (!disposed) await menu.setAsAppMenu();
    })().catch((e) => console.error("Failed to build app menu:", e));
    return () => { disposed = true; };
  }, []);
}
