/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Metacity X screen handler */

/*
 * Copyright (C) 2001, 2002 Havoc Pennington
 * Copyright (C) 2002, 2003 Red Hat Inc.
 * Some ICCCM manager selection code derived from fvwm2,
 * Copyright (C) 2001 Dominik Vogt, Matthias Clasen, and fvwm2 team
 * Copyright (C) 2003 Rob Adams
 * Copyright (C) 2004-2006 Elijah Newren
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include "screen-private.h"
#include "util.h"
#include "errors.h"
#include "window-private.h"
#include "frame-private.h"
#include "prefs.h"
#include "workspace.h"
#include "keybindings.h"
#include "stack.h"
#include "xprops.h"
#include "compositor.h"
#include "deepin-desktop-background.h"
#include "deepin-corner-indicator.h"
#include "deepin-wm-background.h"
#include "deepin-workspace-indicator.h"
#include "deepin-message-hub.h"

#ifdef HAVE_SOLARIS_XINERAMA
#include <X11/extensions/xinerama.h>
#endif
#ifdef HAVE_XFREE_XINERAMA
#include <X11/extensions/Xinerama.h>
#endif

#include <X11/extensions/XInput2.h>

#include <X11/Xatom.h>
#include <locale.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static char* get_screen_name (MetaDisplay *display,
                              int          number);

static void update_num_workspaces  (MetaScreen *screen,
                                    guint32     timestamp);
static void update_focus_mode      (MetaScreen *screen);
static void set_workspace_names    (MetaScreen *screen);
static void prefs_changed_callback (MetaPreference pref,
                                    gpointer       data);

static void set_desktop_geometry_hint (MetaScreen *screen);
static void set_desktop_viewport_hint (MetaScreen *screen);
static void on_screen_changed         (DeepinMessageHub* hub,
                                       MetaScreen* screen,
                                       gpointer data);
static void on_screen_scaled          (DeepinMessageHub* hub,
                                       gdouble scale,
                                       gpointer data);

#ifdef HAVE_STARTUP_NOTIFICATION
static void meta_screen_sn_event   (SnMonitorEvent *event,
                                    void           *user_data);
#endif

static int
set_wm_check_hint (MetaScreen *screen)
{
  unsigned long data[1];

  g_return_val_if_fail (screen->display->leader_window != None, 0);

  data[0] = screen->display->leader_window;

  XChangeProperty (screen->display->xdisplay, screen->xroot,
                   screen->display->atom__NET_SUPPORTING_WM_CHECK,
                   XA_WINDOW,
                   32, PropModeReplace, (guchar*) data, 1);

  return Success;
}

static void
unset_wm_check_hint (MetaScreen *screen)
{
  XDeleteProperty (screen->display->xdisplay, screen->xroot,
                   screen->display->atom__NET_SUPPORTING_WM_CHECK);
}

static int
set_supported_hint (MetaScreen *screen)
{
  Atom atoms[] = {
#define EWMH_ATOMS_ONLY
#define item(x)  screen->display->atom_##x,
#include "atomnames.h"
#undef item
#undef EWMH_ATOMS_ONLY

    screen->display->atom__GTK_FRAME_EXTENTS,
    screen->display->atom__GTK_SHOW_WINDOW_MENU,
  };

  XChangeProperty (screen->display->xdisplay, screen->xroot,
                   screen->display->atom__NET_SUPPORTED,
                   XA_ATOM,
                   32, PropModeReplace,
                   (guchar*) atoms, G_N_ELEMENTS(atoms));

  return Success;
}

static int
set_wm_icon_size_hint (MetaScreen *screen)
{
#define N_VALS 6
  gulong vals[N_VALS];

  /* We've bumped the real icon size up to 96x96, but
   * we really should not add these sorts of constraints
   * on clients still using the legacy WM_HINTS interface.
   */
#define LEGACY_ICON_SIZE 32

  /* min width, min height, max w, max h, width inc, height inc */
  vals[0] = LEGACY_ICON_SIZE;
  vals[1] = LEGACY_ICON_SIZE;
  vals[2] = LEGACY_ICON_SIZE;
  vals[3] = LEGACY_ICON_SIZE;
  vals[4] = 0;
  vals[5] = 0;
#undef LEGACY_ICON_SIZE

  XChangeProperty (screen->display->xdisplay, screen->xroot,
                   screen->display->atom_WM_ICON_SIZE,
                   XA_CARDINAL,
                   32, PropModeReplace, (guchar*) vals, N_VALS);

  return Success;
#undef N_VALS
}

static void
reload_xinerama_infos (MetaScreen *screen)
{
  MetaDisplay *display;

  {
    GList *tmp;

    tmp = screen->workspaces;
    while (tmp != NULL)
      {
        MetaWorkspace *space = tmp->data;

        meta_workspace_invalidate_work_area (space);

        tmp = tmp->next;
      }
  }

  display = screen->display;

  if (screen->xinerama_infos)
    g_free (screen->xinerama_infos);

  screen->xinerama_infos = NULL;
  screen->n_xinerama_infos = 0;
  screen->last_xinerama_index = 0;

  screen->display->xinerama_cache_invalidated = TRUE;

#ifdef HAVE_XFREE_XINERAMA
    {
      int n_infos;
      int i;
      GdkScreen *gscreen;

      gscreen = gdk_screen_get_default ();
      n_infos = gdk_screen_get_n_monitors (gscreen);

      meta_topic (META_DEBUG_XINERAMA,
                  "Found %d Xinerama screens on display %s\n",
                  n_infos, display->name);

      if (n_infos > 0)
        {
          screen->xinerama_infos = g_new (MetaXineramaScreenInfo, n_infos);
          screen->n_xinerama_infos = n_infos;

          i = 0;
          while (i < n_infos)
            {
              GdkRectangle r;
              gdk_screen_get_monitor_geometry (gscreen, i, &r);

              screen->xinerama_infos[i].number = i;
              screen->xinerama_infos[i].rect = *(MetaRectangle*) &r;

              meta_topic (META_DEBUG_XINERAMA,
                          "Xinerama %d is %d,%d %d x %d\n",
                          screen->xinerama_infos[i].number,
                          screen->xinerama_infos[i].rect.x,
                          screen->xinerama_infos[i].rect.y,
                          screen->xinerama_infos[i].rect.width,
                          screen->xinerama_infos[i].rect.height);

              ++i;
            }
        }
    }
#else
  meta_topic (META_DEBUG_XINERAMA,
              "Metacity compiled without XFree86 Xinerama support\n");
#endif /* HAVE_XFREE_XINERAMA */

#ifdef HAVE_SOLARIS_XINERAMA
  /* This code from GDK, Copyright (C) 2002 Sun Microsystems */
  if (screen->n_xinerama_infos == 0 &&
      XineramaGetState (screen->display->xdisplay,
                        screen->number))
    {
      XRectangle monitors[MAXFRAMEBUFFERS];
      unsigned char hints[16];
      int result;
      int n_monitors;
      int i;

      n_monitors = 0;
      result = XineramaGetInfo (screen->display->xdisplay,
                                screen->number,
				monitors, hints,
                                &n_monitors);
      /* Yes I know it should be Success but the current implementation
       * returns the num of monitor
       */
      if (result > 0)
	{
          g_assert (n_monitors > 0);

          screen->xinerama_infos = g_new (MetaXineramaScreenInfo, n_monitors);
          screen->n_xinerama_infos = n_monitors;

          i = 0;
          while (i < n_monitors)
            {
              screen->xinerama_infos[i].number = i;
              screen->xinerama_infos[i].rect.x = monitors[i].x;
              screen->xinerama_infos[i].rect.y = monitors[i].y;
              screen->xinerama_infos[i].rect.width = monitors[i].width;
              screen->xinerama_infos[i].rect.height = monitors[i].height;

              meta_topic (META_DEBUG_XINERAMA,
                          "Xinerama %d is %d,%d %d x %d\n",
                          screen->xinerama_infos[i].number,
                          screen->xinerama_infos[i].rect.x,
                          screen->xinerama_infos[i].rect.y,
                          screen->xinerama_infos[i].rect.width,
                          screen->xinerama_infos[i].rect.height);

              ++i;
            }
	}
    }
  else if (screen->n_xinerama_infos == 0)
    {
      meta_topic (META_DEBUG_XINERAMA,
                  "No Solaris Xinerama extension or Solaris Xinerama inactive on display %s\n",
                  display->name);
    }
#else
  meta_topic (META_DEBUG_XINERAMA,
              "Metacity compiled without Solaris Xinerama support\n");
#endif /* HAVE_SOLARIS_XINERAMA */


  /* If no Xinerama, fill in the single screen info so
   * we can use the field unconditionally
   */
  if (screen->n_xinerama_infos == 0)
    {
      if (g_getenv ("METACITY_DEBUG_XINERAMA"))
        {
          meta_topic (META_DEBUG_XINERAMA,
                      "Pretending a single monitor has two Xinerama screens\n");

          screen->xinerama_infos = g_new (MetaXineramaScreenInfo, 2);
          screen->n_xinerama_infos = 2;

          screen->xinerama_infos[0].number = 0;
          screen->xinerama_infos[0].rect = screen->rect;
          screen->xinerama_infos[0].rect.width = screen->rect.width / 2;

          screen->xinerama_infos[1].number = 1;
          screen->xinerama_infos[1].rect = screen->rect;
          screen->xinerama_infos[1].rect.x = screen->rect.width / 2;
          screen->xinerama_infos[1].rect.width = screen->rect.width / 2;
        }
      else
        {
          meta_topic (META_DEBUG_XINERAMA,
                      "No Xinerama screens, using default screen info\n");

          screen->xinerama_infos = g_new (MetaXineramaScreenInfo, 1);
          screen->n_xinerama_infos = 1;

          screen->xinerama_infos[0].number = 0;
          screen->xinerama_infos[0].rect = screen->rect;
        }
    }

  g_assert (screen->n_xinerama_infos > 0);
  g_assert (screen->xinerama_infos != NULL);
}

/* The guard window allows us to leave minimized windows mapped so
 * that compositor code may provide live previews of them.
 * Instead of being unmapped/withdrawn, they get pushed underneath
 * the guard window. We also select events on the guard window, which
 * should effectively be forwarded to events on the background actor,
 * providing that the scene graph is set up correctly.
 */
static Window
create_guard_window (Display *xdisplay, MetaScreen *screen)
{
  XSetWindowAttributes attributes;
  Window guard_window;
  gulong create_serial;

  attributes.event_mask = NoEventMask;
  attributes.override_redirect = True;

  /* We have to call record_add() after we have the new window ID,
   * so save the serial for the CreateWindow request until then */
  create_serial = XNextRequest(xdisplay);
  guard_window =
    XCreateWindow (xdisplay,
		   screen->xroot,
		   0, /* x */
		   0, /* y */
		   screen->rect.width,
		   screen->rect.height,
		   0, /* border width */
		   0, /* depth */
		   InputOnly, /* class */
		   CopyFromParent, /* visual */
		   CWEventMask|CWOverrideRedirect,
		   &attributes);

  /* https://bugzilla.gnome.org/show_bug.cgi?id=710346 */
  XStoreName (xdisplay, guard_window, "deepin metacity guard window");

  {
    unsigned char mask_bits[XIMaskLen (XI_LASTEVENT)] = { 0 };
    XIEventMask mask = { XIAllMasterDevices, sizeof (mask_bits), mask_bits };

    XISetMask (mask.mask, XI_ButtonPress);
    XISetMask (mask.mask, XI_ButtonRelease);
    XISetMask (mask.mask, XI_Motion);
    XISelectEvents (xdisplay, guard_window, &mask, 1);
  }

  XLowerWindow (xdisplay, guard_window);
  XMapWindow (xdisplay, guard_window);
  return guard_window;
}

#define CORNER_BASE_SIZE 39

static void meta_screen_calc_corner_positions (MetaScreen *screen, int* positions)
{
    int width = screen->rect.width, height = screen->rect.height;
    int CORNER_SIZE = CORNER_BASE_SIZE * deepin_message_hub_get_screen_scale ();

    int n = gdk_screen_get_n_monitors(gdk_screen_get_default());
    int tl_x, tl_y, tr_x, tr_y, bl_x, bl_y, br_x, br_y;

    tl_x = 0;
    tl_y = height;
    bl_x = 0;
    bl_y = 0;
    for (int i = 0; i < n; i++) {
        // test if monitor is on the left
        GdkRectangle geo;
        gdk_screen_get_monitor_geometry(gdk_screen_get_default(), i, &geo);
        if (geo.x != 0) {
            continue;
        }

        tl_y = MIN (geo.y, tl_y);
        bl_y = MAX (geo.y + geo.height, bl_y);
    }


    tr_x = width;
    tr_y = height;
    br_x = width;
    br_y = 0;
    for (int i = 0; i < n; i++) {
        // test if monitor is on the right
        GdkRectangle geo;
        gdk_screen_get_monitor_geometry(gdk_screen_get_default(), i, &geo);
        if (geo.x + geo.width != width) {
            continue;
        }

        tr_y = MIN (geo.y, tr_y);
        br_y = MAX (geo.y + geo.height, br_y);
    }

    int result[] = {
        tl_x, tl_y,
        tr_x - CORNER_SIZE, tr_y,
        bl_x, bl_y - CORNER_SIZE, 
        br_x - CORNER_SIZE, br_y - CORNER_SIZE,
    };

    meta_verbose ("%s: (%d, %d), (%d, %d), (%d, %d), (%d, %d)\n", __func__,
            tl_x, tl_y, tr_x, tr_y, bl_x, bl_y, br_x, br_y);

    memcpy (positions, result, sizeof result);
}

static Window
create_screen_corner_window (Display *xdisplay, MetaScreen *screen, MetaScreenCorner corner, int x, int y)
{
  XSetWindowAttributes attributes;
  Window edge_window;
  gulong create_serial;
  int w = CORNER_BASE_SIZE * deepin_message_hub_get_screen_scale (),
      h = CORNER_BASE_SIZE * deepin_message_hub_get_screen_scale ();

  attributes.event_mask = PointerMotionMask | EnterWindowMask | LeaveWindowMask;
  attributes.override_redirect = True;

  /* We have to call record_add() after we have the new window ID,
   * so save the serial for the CreateWindow request until then */
  create_serial = XNextRequest(xdisplay);
  edge_window =
    XCreateWindow (xdisplay,
		   screen->xroot,
           //FIXME: topleft now
		   x, /* x */
		   y, /* y */
		   w,
		   h,
		   0, /* border width */
		   0, /* depth */
		   InputOnly, /* class */
		   CopyFromParent, /* visual */
		   CWEventMask|CWOverrideRedirect,
		   &attributes);

  XStoreName (xdisplay, edge_window, "mutter corner window");

  {
    unsigned char mask_bits[XIMaskLen (XI_LASTEVENT)] = { 0 };
    XIEventMask mask = { XIAllMasterDevices, sizeof (mask_bits), mask_bits };

    XISetMask (mask.mask, XI_Enter);
    XISetMask (mask.mask, XI_Leave);
    XISetMask (mask.mask, XI_Motion);

    /* Sync on the connection we created the window on to
     * make sure it's created before we select on it on the
     * backend connection. */
    XSync (xdisplay, False);

    XISelectEvents (xdisplay, edge_window, &mask, 1);
  }

  {
      XWindowChanges changes;

      meta_error_trap_push (screen->display);

      changes.stack_mode = Above;
      XConfigureWindow (xdisplay, edge_window, CWStackMode, &changes);

      meta_error_trap_pop (screen->display, FALSE);
  }

  XMapWindow (xdisplay, edge_window);
  return edge_window;
}

static DeepinDesktopBackground* create_desktop_background(MetaScreen* screen, gint monitor)
{
    GtkWidget* widget = (GtkWidget*)deepin_desktop_background_new(screen, monitor);

    gtk_widget_realize (widget);

    gdk_window_lower(gtk_widget_get_window(widget));
    gtk_widget_show_all(widget);

    return DEEPIN_DESKTOP_BACKGROUND(widget);
}

MetaScreen*
meta_screen_new (MetaDisplay *display,
                 int          number,
                 guint32      timestamp)
{
  MetaScreen *screen;
  Window xroot;
  Display *xdisplay;
  XWindowAttributes attr;
  Window new_wm_sn_owner;
  Window current_wm_sn_owner;
  gboolean replace_current_wm;
  Atom wm_sn_atom;
  char buf[128];
  guint32 manager_timestamp;
  gulong current_workspace;

  replace_current_wm = meta_get_replace_current_wm ();

  /* Only display->name, display->xdisplay, and display->error_traps
   * can really be used in this function, since normally screens are
   * created from the MetaDisplay constructor
   */

  xdisplay = display->xdisplay;

  meta_verbose ("Trying screen %d on display '%s'\n",
                number, display->name);

  xroot = RootWindow (xdisplay, number);

  /* FVWM checks for None here, I don't know if this
   * ever actually happens
   */
  if (xroot == None)
    {
      meta_warning (_("Screen %d on display '%s' is invalid\n"),
                    number, display->name);
      return NULL;
    }

  sprintf (buf, "WM_S%d", number);
  wm_sn_atom = XInternAtom (xdisplay, buf, False);

  current_wm_sn_owner = XGetSelectionOwner (xdisplay, wm_sn_atom);

  if (current_wm_sn_owner != None)
    {
      XSetWindowAttributes attrs;

      if (!replace_current_wm)
        {
          meta_warning (_("Screen %d on display \"%s\" already has a window manager; try using the --replace option to replace the current window manager.\n"),
                        number, display->name);

          return NULL;
        }

      /* We want to find out when the current selection owner dies */
      meta_error_trap_push_with_return (display);
      attrs.event_mask = StructureNotifyMask;
      XChangeWindowAttributes (xdisplay,
                               current_wm_sn_owner, CWEventMask, &attrs);
      if (meta_error_trap_pop_with_return (display, FALSE) != Success)
        current_wm_sn_owner = None; /* don't wait for it to die later on */
    }

  /* We need SelectionClear and SelectionRequest events on the new_wm_sn_owner,
   * but those cannot be masked, so we only need NoEventMask.
   */
  new_wm_sn_owner = meta_create_offscreen_window (xdisplay, xroot, NoEventMask);

  manager_timestamp = timestamp;

  XSetSelectionOwner (xdisplay, wm_sn_atom, new_wm_sn_owner,
                      manager_timestamp);

  if (XGetSelectionOwner (xdisplay, wm_sn_atom) != new_wm_sn_owner)
    {
      meta_warning (_("Could not acquire window manager selection on screen %d display \"%s\"\n"),
                    number, display->name);

      XDestroyWindow (xdisplay, new_wm_sn_owner);

      return NULL;
    }

  {
    /* Send client message indicating that we are now the WM */
    XClientMessageEvent ev;

    ev.type = ClientMessage;
    ev.window = xroot;
    ev.message_type = display->atom_MANAGER;
    ev.format = 32;
    ev.data.l[0] = manager_timestamp;
    ev.data.l[1] = wm_sn_atom;

    XSendEvent (xdisplay, xroot, False, StructureNotifyMask, (XEvent*)&ev);
  }

  /* Wait for old window manager to go away */
  if (current_wm_sn_owner != None)
    {
      XEvent event;

      /* We sort of block infinitely here which is probably lame. */

      meta_verbose ("Waiting for old window manager to exit\n");
      do
        {
          XWindowEvent (xdisplay, current_wm_sn_owner, StructureNotifyMask, &event);
          g_usleep(G_USEC_PER_SEC / 10);
        }
      while (event.type != DestroyNotify);
      g_usleep(G_USEC_PER_SEC * 3);
    }

  /* select our root window events */
  meta_error_trap_push_with_return (display);

  {
    unsigned char mask_bits[XIMaskLen (XI_LASTEVENT)] = { 0 };
    XIEventMask mask = { XIAllMasterDevices, sizeof (mask_bits), mask_bits };

    XISetMask (mask.mask, XI_KeyPress);
    XISetMask (mask.mask, XI_KeyRelease);
    XISetMask (mask.mask, XI_Enter);
    XISetMask (mask.mask, XI_Leave);
    XISetMask (mask.mask, XI_FocusIn);
    XISetMask (mask.mask, XI_FocusOut);

    XISelectEvents (xdisplay, xroot, &mask, 1);

  }

  /* We need to or with the existing event mask since
   * gtk+ may be interested in other events.
   */
  XGetWindowAttributes (xdisplay, xroot, &attr);
  XSelectInput (xdisplay,
                xroot,
                SubstructureRedirectMask | SubstructureNotifyMask |
                ColormapChangeMask | PropertyChangeMask |
                StructureNotifyMask |
#ifdef HAVE_COMPOSITE_EXTENSIONS
                ExposureMask |
#endif
		attr.your_event_mask);
  if (meta_error_trap_pop_with_return (display, FALSE) != Success)
    {
      meta_warning (_("Screen %d on display \"%s\" already has a window manager\n"),
                    number, display->name);

      XDestroyWindow (xdisplay, new_wm_sn_owner);

      return NULL;
    }


  screen = g_new (MetaScreen, 1);
  screen->closing = 0;

  screen->display = display;
  screen->number = number;
  screen->screen_name = get_screen_name (display, number);
  screen->xscreen = ScreenOfDisplay (xdisplay, number);
  screen->xroot = xroot;
  screen->rect.x = screen->rect.y = 0;
  screen->rect.width = WidthOfScreen (screen->xscreen);
  screen->rect.height = HeightOfScreen (screen->xscreen);
  screen->current_cursor = -1; /* invalid/unset */
  screen->default_xvisual = DefaultVisualOfScreen (screen->xscreen);
  screen->default_depth = DefaultDepthOfScreen (screen->xscreen);
  screen->flash_window = None;

  screen->wm_sn_selection_window = new_wm_sn_owner;
  screen->wm_sn_atom = wm_sn_atom;
  screen->wm_sn_timestamp = manager_timestamp;

#ifdef HAVE_COMPOSITE_EXTENSIONS
  screen->wm_cm_selection_window = meta_create_offscreen_window (xdisplay,
                                                                 xroot,
                                                                 NoEventMask);
#endif
  screen->work_area_idle = 0;

  screen->active_workspace = NULL;
  screen->workspaces = NULL;
  screen->rows_of_workspaces = 1;
  screen->columns_of_workspaces = -1;
  screen->vertical_workspaces = FALSE;
  screen->starting_corner = META_SCREEN_TOPLEFT;
  screen->compositor_data = NULL;
  screen->guard_window = None;
  screen->corner_windows[0] = None;
  screen->corner_windows[1] = None;
  screen->corner_windows[2] = None;
  screen->corner_windows[3] = None;
  screen->corner_actions_enabled = TRUE;
  screen->corner_enabled[0] = TRUE;
  screen->corner_enabled[1] = TRUE;
  screen->corner_enabled[2] = TRUE;
  screen->corner_enabled[3] = TRUE;
  screen->desktop_bgs = NULL;
  screen->desktop_bg_windows = NULL;

  {
    XFontStruct *font_info;
    XGCValues gc_values;
    gulong value_mask = 0;

    gc_values.subwindow_mode = IncludeInferiors;
    value_mask |= GCSubwindowMode;
    gc_values.function = GXinvert;
    value_mask |= GCFunction;
    gc_values.line_width = META_WIREFRAME_XOR_LINE_WIDTH;
    value_mask |= GCLineWidth;

    font_info = XLoadQueryFont (screen->display->xdisplay, "fixed");

    if (font_info != NULL)
      {
        gc_values.font = font_info->fid;
        value_mask |= GCFont;
        XFreeFontInfo (NULL, font_info, 1);
      }
    else
      meta_warning ("xserver doesn't have 'fixed' font.\n");

    screen->root_xor_gc = XCreateGC (screen->display->xdisplay,
                                     screen->xroot,
                                     value_mask,
                                     &gc_values);
  }

  screen->xinerama_infos = NULL;
  screen->n_xinerama_infos = 0;
  screen->last_xinerama_index = 0;

  reload_xinerama_infos (screen);

  meta_screen_set_cursor (screen, META_CURSOR_DEFAULT);

  /* Handle creating a no_focus_window for this screen */
  screen->no_focus_window =
    meta_create_offscreen_window (display->xdisplay,
                                  screen->xroot,
                                  FocusChangeMask|KeyPressMask|KeyReleaseMask);
  XMapWindow (display->xdisplay, screen->no_focus_window);
  /* Done with no_focus_window stuff */

  set_wm_icon_size_hint (screen);

  set_supported_hint (screen);

  set_wm_check_hint (screen);

  set_desktop_viewport_hint (screen);

  set_desktop_geometry_hint (screen);

  meta_screen_update_workspace_layout (screen);

  /* Get current workspace */
  current_workspace = 0;
  if (meta_prop_get_cardinal (screen->display,
                              screen->xroot,
                              screen->display->atom__NET_CURRENT_DESKTOP,
                              &current_workspace))
    meta_verbose ("Read existing _NET_CURRENT_DESKTOP = %d\n",
                  (int) current_workspace);
  else
    meta_verbose ("No _NET_CURRENT_DESKTOP present\n");

  /* Screens must have at least one workspace at all times,
   * so create that required workspace.
   */
  meta_workspace_activate (meta_workspace_new (screen), timestamp);
  update_num_workspaces (screen, timestamp);

  set_workspace_names (screen);

  screen->all_keys_grabbed = FALSE;
  screen->keys_grabbed = FALSE;
  meta_screen_grab_keys (screen);

  screen->ui = meta_ui_new (screen->display->xdisplay,
                            screen->xscreen);

  screen->tab_popup = NULL;
  screen->ws_previewer = NULL;
  screen->exposing_windows_popup = NULL;
  screen->tile_preview = NULL;
  screen->workspace_indicator = NULL;

  screen->tile_preview_timeout_id = 0;

  screen->stack = meta_stack_new (screen);

  meta_prefs_add_listener (prefs_changed_callback, screen);

#ifdef HAVE_STARTUP_NOTIFICATION
  screen->sn_context =
    sn_monitor_context_new (screen->display->sn_display,
                            screen->number,
                            meta_screen_sn_event,
                            screen,
                            NULL);
  screen->startup_sequences = NULL;
  screen->startup_sequence_timeout = 0;
#endif

  /* Switch to the _NET_CURRENT_DESKTOP workspace */
  {
    MetaWorkspace *space;

    space = meta_screen_get_workspace_by_index (screen,
                                                current_workspace);

    if (space != NULL)
      meta_workspace_activate (space, timestamp);
  }

  g_object_connect(G_OBJECT(deepin_message_hub_get()),
          "signal::screen-changed", (GCallback)on_screen_changed, NULL,
          "signal::screen-scaled", (GCallback)on_screen_scaled, screen, 
          NULL);

  MetaScreenCorner corners[] = {
      META_SCREEN_TOPLEFT,
      META_SCREEN_TOPRIGHT,
      META_SCREEN_BOTTOMLEFT,
      META_SCREEN_BOTTOMRIGHT,
  };

  const char *keys[] = {
      "left-up", "right-up", "left-down", "right-down" 
  };

  int positions[8];
  meta_screen_calc_corner_positions (screen, positions);

  int i;
  for (i = 0; i < 4; i++) 
  {
    screen->corner_indicator[i] = deepin_corner_indicator_new (
            screen, corners[i], keys[i], positions[i*2], positions[i*2+1]);
    gtk_widget_show (screen->corner_indicator[i]);
  }

  meta_verbose ("Added screen %d ('%s') root 0x%lx\n",
                screen->number, screen->screen_name, screen->xroot);

  return screen;
}

void
meta_screen_free (MetaScreen *screen,
                  guint32     timestamp)
{
  MetaDisplay *display;
  XGCValues gc_values = { 0 };

  display = screen->display;

  screen->closing += 1;

  if (screen->display->compositor)
    {
      meta_compositor_unmanage_screen (screen->display->compositor,
				       screen);
    }

  meta_display_unmanage_windows_for_screen (display, screen, timestamp);

  meta_prefs_remove_listener (prefs_changed_callback, screen);

  meta_screen_ungrab_keys (screen);

#ifdef HAVE_STARTUP_NOTIFICATION
  g_slist_foreach (screen->startup_sequences,
                   (GFunc) sn_startup_sequence_unref, NULL);
  g_slist_free (screen->startup_sequences);
  screen->startup_sequences = NULL;

  if (screen->startup_sequence_timeout != 0)
    {
      g_source_remove (screen->startup_sequence_timeout);
      screen->startup_sequence_timeout = 0;
    }
  if (screen->sn_context)
    {
      sn_monitor_context_unref (screen->sn_context);
      screen->sn_context = NULL;
    }
#endif

  meta_ui_free (screen->ui);

  meta_stack_free (screen->stack);

  meta_error_trap_push_with_return (screen->display);
  XSelectInput (screen->display->xdisplay, screen->xroot, 0);
  if (meta_error_trap_pop_with_return (screen->display, FALSE) != Success)
    meta_warning (_("Could not release screen %d on display \"%s\"\n"),
                  screen->number, screen->display->name);

  unset_wm_check_hint (screen);

  XDestroyWindow (screen->display->xdisplay,
                  screen->wm_sn_selection_window);

  if (screen->work_area_idle != 0)
    g_source_remove (screen->work_area_idle);


  if (XGetGCValues (screen->display->xdisplay,
                    screen->root_xor_gc,
                    GCFont,
                    &gc_values))
    {
      XUnloadFont (screen->display->xdisplay,
                   gc_values.font);
    }

  XFreeGC (screen->display->xdisplay,
           screen->root_xor_gc);

  if (screen->xinerama_infos)
    g_free (screen->xinerama_infos);

  if (screen->tile_preview_timeout_id)
    g_source_remove (screen->tile_preview_timeout_id);

  if (screen->tile_preview)
    meta_tile_preview_free (screen->tile_preview);

  g_free (screen->screen_name);
  g_free (screen);

  XFlush (display->xdisplay);
}

typedef struct
{
  Window		xwindow;
  XWindowAttributes	attrs;
} WindowInfo;

static GList *
list_windows (MetaScreen *screen)
{
  Window ignored1, ignored2;
  Window *children;
  guint n_children, i;
  GList *result;

  XQueryTree (screen->display->xdisplay,
              screen->xroot,
              &ignored1, &ignored2, &children, &n_children);

  result = NULL;
  for (i = 0; i < n_children; ++i)
    {
      WindowInfo *info = g_new0 (WindowInfo, 1);

      meta_error_trap_push_with_return (screen->display);

      XGetWindowAttributes (screen->display->xdisplay,
                            children[i], &info->attrs);

      if (meta_error_trap_pop_with_return (screen->display, TRUE))
	{
          meta_verbose ("Failed to get attributes for window 0x%lx\n",
                        children[i]);
	  g_free (info);
        }
      else
        {
	  info->xwindow = children[i];
	}

      result = g_list_prepend (result, info);
    }

  if (children)
    XFree (children);

  return g_list_reverse (result);
}

static void meta_screen_rebuild_backgrounds (MetaScreen *screen)
{
  if (screen->desktop_bgs) {
    g_clear_pointer (&screen->desktop_bgs, g_ptr_array_unref);
    g_clear_pointer (&screen->desktop_bg_windows, g_array_unref);
  }

  gint n_monitors = gdk_screen_get_n_monitors(gdk_screen_get_default());
  screen->desktop_bgs = g_ptr_array_new_full(n_monitors,
          (GDestroyNotify)gtk_widget_destroy);
  screen->desktop_bg_windows = g_array_sized_new(FALSE, FALSE, sizeof(Window), n_monitors);

  int primary = gdk_screen_get_primary_monitor(gdk_screen_get_default());
  for (int monitor = 0; monitor < n_monitors; monitor++) {
      DeepinDesktopBackground* widget = create_desktop_background(screen, monitor);
      Window xid = GDK_WINDOW_XID(gtk_widget_get_window(widget));
      /**
       * keep primary on top, since we only render the desktop surface into the 
       * primary desktop window
       */
      if (monitor == primary) {
          g_array_prepend_val(screen->desktop_bg_windows, xid);
          g_ptr_array_insert(screen->desktop_bgs, 0, widget);
      } else {
          g_array_append_val(screen->desktop_bg_windows, xid);
          g_ptr_array_add(screen->desktop_bgs, widget);
      }
  }

  if (screen->desktop_bg_windows->len) {
      XLowerWindow (screen->display->xdisplay, *(Window*)screen->desktop_bg_windows->data);
      XRestackWindows (screen->display->xdisplay,
              (Window *)screen->desktop_bg_windows->data,
              screen->desktop_bg_windows->len);
  }
}

void
meta_screen_manage_all_windows (MetaScreen *screen)
{
  GList *windows;
  GList *list;

  if (screen->guard_window == None)
    screen->guard_window = create_guard_window (screen->display->xdisplay,
                                                screen);
  if (screen->corner_windows[0] == None) {
      int i;
      MetaScreenCorner corners[] = {
          META_SCREEN_TOPLEFT,
          META_SCREEN_TOPRIGHT,
          META_SCREEN_BOTTOMLEFT,
          META_SCREEN_BOTTOMRIGHT,
      };

      int positions[8];
      meta_screen_calc_corner_positions (screen, positions);

      for (i = 0; i < 4; i++) 
      {
          screen->corner_windows[i] = create_screen_corner_window (
                  screen->display->xdisplay, screen, corners[i], positions[i*2], positions[i*2+1]);
      }
  }

  meta_screen_rebuild_backgrounds (screen);

  meta_display_grab (screen->display);

  windows = list_windows (screen);

  meta_stack_freeze (screen->stack);
  for (list = windows; list != NULL; list = list->next)
    {
      WindowInfo *info = list->data;
      MetaWindow *window;

      window = meta_window_new_with_attrs (screen->display, info->xwindow, TRUE,
                                           &info->attrs);
      if (info->xwindow == screen->no_focus_window ||
          info->xwindow == screen->flash_window ||
#ifdef HAVE_COMPOSITE_EXTENSIONS
          info->xwindow == screen->wm_cm_selection_window ||
#endif
          info->xwindow == screen->wm_sn_selection_window) {
        meta_verbose ("Not managing our own windows\n");
        continue;
      }

      int i;
      for (i = 0; i < screen->desktop_bg_windows->len; i++) 
        {
          Window xid = g_array_index(screen->desktop_bg_windows, Window, i);
          if (info->xwindow == xid)
            {
              meta_verbose ("Not managing our own windows\n");
              continue;
            }
        }

      if (screen->display->compositor)
        meta_compositor_add_window (screen->display->compositor, window,
                                    info->xwindow, &info->attrs);
    }
  meta_stack_thaw (screen->stack);

  g_list_foreach (windows, (GFunc)g_free, NULL);
  g_list_free (windows);

  meta_display_ungrab (screen->display);
}

void
meta_screen_composite_all_windows (MetaScreen *screen)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  MetaDisplay *display;
  GList *windows, *list;

  display = screen->display;
  if (!display->compositor)
    return;

  windows = list_windows (screen);

  meta_stack_freeze (screen->stack);

  for (list = windows; list != NULL; list = list->next)
    {
      WindowInfo *info = list->data;

      if (info->xwindow == screen->no_focus_window ||
          info->xwindow == screen->flash_window ||
          info->xwindow == screen->wm_sn_selection_window ||
          info->xwindow == screen->wm_cm_selection_window) {
        meta_verbose ("Not managing our own windows\n");
        continue;
      }

      int i;
      for (i = 0; i < screen->desktop_bg_windows->len; i++) 
        {
          Window xid = g_array_index(screen->desktop_bg_windows, Window, i);
          if (info->xwindow == xid)
            {
              meta_verbose ("Not managing our own windows\n");
              continue;
            }
        }

      meta_compositor_add_window (display->compositor,
                                  meta_display_lookup_x_window (display,
                                                                info->xwindow),
				  info->xwindow, &info->attrs);
    }

  meta_stack_thaw (screen->stack);

  g_list_foreach (windows, (GFunc)g_free, NULL);
  g_list_free (windows);
#endif
}

MetaScreen*
meta_screen_for_x_screen (Screen *xscreen)
{
  MetaDisplay *display;

  display = meta_display_for_x_display (DisplayOfScreen (xscreen));

  if (display == NULL)
    return NULL;

  return meta_display_screen_for_x_screen (display, xscreen);
}

static void
prefs_changed_callback (MetaPreference pref,
                        gpointer       data)
{
  MetaScreen *screen = data;

  if (pref == META_PREF_NUM_WORKSPACES)
    {
      /* GSettings doesn't provide timestamps, but luckily update_num_workspaces
       * often doesn't need it...
       */
      guint32 timestamp =
        meta_display_get_current_time_roundtrip (screen->display);
      update_num_workspaces (screen, timestamp);
    }
  else if (pref == META_PREF_FOCUS_MODE)
    {
      update_focus_mode (screen);
    }
  else if (pref == META_PREF_WORKSPACE_NAMES)
    {
      set_workspace_names (screen);
    }
}


static char*
get_screen_name (MetaDisplay *display,
                 int          number)
{
  char *p;
  char *dname;
  char *scr;

  /* DisplayString gives us a sort of canonical display,
   * vs. the user-entered name from XDisplayName()
   */
  dname = g_strdup (DisplayString (display->xdisplay));

  /* Change display name to specify this screen.
   */
  p = strrchr (dname, ':');
  if (p)
    {
      p = strchr (p, '.');
      if (p)
        *p = '\0';
    }

  scr = g_strdup_printf ("%s.%d", dname, number);

  g_free (dname);

  return scr;
}

static gint
ptrcmp (gconstpointer a, gconstpointer b)
{
  if (a < b)
    return -1;
  else if (a > b)
    return 1;
  else
    return 0;
}

static void
listify_func (gpointer key, gpointer value, gpointer data)
{
  GSList **listp;

  listp = data;

  *listp = g_slist_prepend (*listp, value);
}

void
meta_screen_foreach_window (MetaScreen *screen,
                            MetaScreenWindowFunc func,
                            gpointer data)
{
  GSList *winlist;
  GSList *tmp;

  /* If we end up doing this often, just keeping a list
   * of windows might be sensible.
   */

  winlist = NULL;
  g_hash_table_foreach (screen->display->window_ids,
                        listify_func,
                        &winlist);

  winlist = g_slist_sort (winlist, ptrcmp);

  tmp = winlist;
  while (tmp != NULL)
    {
      /* If the next node doesn't contain this window
       * a second time, delete the window.
       */
      if (tmp->next == NULL ||
          (tmp->next && tmp->next->data != tmp->data))
        {
          MetaWindow *window = tmp->data;

          if (window->screen == screen)
            (* func) (screen, window, data);
        }

      tmp = tmp->next;
    }
  g_slist_free (winlist);
}

static void
queue_draw (MetaScreen *screen, MetaWindow *window, gpointer data)
{
  if (window->frame)
    meta_frame_queue_draw (window->frame);
}

void
meta_screen_queue_frame_redraws (MetaScreen *screen)
{
  meta_screen_foreach_window (screen, queue_draw, NULL);
}

static void
queue_resize (MetaScreen *screen, MetaWindow *window, gpointer data)
{
  meta_window_queue (window, META_QUEUE_MOVE_RESIZE);
}

void
meta_screen_queue_window_resizes (MetaScreen *screen)
{
  meta_screen_foreach_window (screen, queue_resize, NULL);
}

int
meta_screen_get_n_workspaces (MetaScreen *screen)
{
  return g_list_length (screen->workspaces);
}

MetaWorkspace*
meta_screen_get_workspace_by_index (MetaScreen  *screen,
                                    int          idx)
{
  GList *tmp;
  int i;

  /* should be robust, idx is maybe from an app */
  if (idx < 0)
    return NULL;

  i = 0;
  tmp = screen->workspaces;
  while (tmp != NULL)
    {
      MetaWorkspace *w = tmp->data;

      if (i == idx)
        return w;

      ++i;
      tmp = tmp->next;
    }

  return NULL;
}

static void
set_number_of_spaces_hint (MetaScreen *screen,
			   int         n_spaces)
{
  unsigned long data[1];

  if (screen->closing > 0)
    return;

  data[0] = n_spaces;

  meta_verbose ("Setting _NET_NUMBER_OF_DESKTOPS to %lu\n", data[0]);

  meta_error_trap_push (screen->display);
  XChangeProperty (screen->display->xdisplay, screen->xroot,
                   screen->display->atom__NET_NUMBER_OF_DESKTOPS,
                   XA_CARDINAL,
                   32, PropModeReplace, (guchar*) data, 1);
  meta_error_trap_pop (screen->display, FALSE);
}

static void
set_desktop_geometry_hint (MetaScreen *screen)
{
  unsigned long data[2];

  if (screen->closing > 0)
    return;

  data[0] = screen->rect.width;
  data[1] = screen->rect.height;

  meta_verbose ("Setting _NET_DESKTOP_GEOMETRY to %lu, %lu\n", data[0], data[1]);

  meta_error_trap_push (screen->display);
  XChangeProperty (screen->display->xdisplay, screen->xroot,
                   screen->display->atom__NET_DESKTOP_GEOMETRY,
                   XA_CARDINAL,
                   32, PropModeReplace, (guchar*) data, 2);
  meta_error_trap_pop (screen->display, FALSE);
}

static void
set_desktop_viewport_hint (MetaScreen *screen)
{
  unsigned long data[2];

  if (screen->closing > 0)
    return;

  /*
   * Metacity does not implement viewports, so this is a fixed 0,0
   */
  data[0] = 0;
  data[1] = 0;

  meta_verbose ("Setting _NET_DESKTOP_VIEWPORT to 0, 0\n");

  meta_error_trap_push (screen->display);
  XChangeProperty (screen->display->xdisplay, screen->xroot,
                   screen->display->atom__NET_DESKTOP_VIEWPORT,
                   XA_CARDINAL,
                   32, PropModeReplace, (guchar*) data, 2);
  meta_error_trap_pop (screen->display, FALSE);
}

static void
update_num_workspaces (MetaScreen *screen,
                       guint32     timestamp)
{
  int new_num;
  GList *tmp;
  int i;
  GList *extras;
  MetaWorkspace *last_remaining;
  gboolean need_change_space;

  new_num = meta_prefs_get_num_workspaces ();

  g_assert (new_num > 0);

  last_remaining = NULL;
  extras = NULL;
  i = 0;
  tmp = screen->workspaces;
  while (tmp != NULL)
    {
      MetaWorkspace *w = tmp->data;

      if (i >= new_num)
        extras = g_list_prepend (extras, w);
      else
        last_remaining = w;

      ++i;
      tmp = tmp->next;
    }

  g_assert (last_remaining);

  /* Get rid of the extra workspaces by moving all their windows
   * to last_remaining, then activating last_remaining if
   * one of the removed workspaces was active. This will be a bit
   * wacky if the config tool for changing number of workspaces
   * is on a removed workspace ;-)
   */
  need_change_space = FALSE;
  tmp = extras;
  while (tmp != NULL)
    {
      MetaWorkspace *w = tmp->data;

      meta_workspace_relocate_windows (w, last_remaining);

      if (w == screen->active_workspace)
        need_change_space = TRUE;

      tmp = tmp->next;
    }

  if (need_change_space)
    meta_workspace_activate (last_remaining, timestamp);

  /* Should now be safe to free the workspaces */
  tmp = extras;
  while (tmp != NULL)
    {
      MetaWorkspace *w = tmp->data;

      g_assert (w->windows == NULL);
      meta_workspace_free (w);

      tmp = tmp->next;
    }

  g_list_free (extras);

  while (i < new_num)
    {
      meta_workspace_new (screen);
      ++i;
    }

  set_number_of_spaces_hint (screen, new_num);

  meta_screen_queue_workarea_recalc (screen);
}

static void
update_focus_mode (MetaScreen *screen)
{
  /* nothing to do anymore */ ;
}

void
meta_screen_set_cursor (MetaScreen *screen,
                        MetaCursor  cursor)
{
  Cursor xcursor;

  if (cursor == screen->current_cursor)
    return;

  screen->current_cursor = cursor;

  xcursor = meta_display_create_x_cursor (screen->display, cursor);
  XDefineCursor (screen->display->xdisplay, screen->xroot, xcursor);
  XFlush (screen->display->xdisplay);
  XFreeCursor (screen->display->xdisplay, xcursor);
}

void
meta_screen_update_cursor (MetaScreen *screen)
{
  Cursor xcursor;

  xcursor = meta_display_create_x_cursor (screen->display,
					  screen->current_cursor);
  XDefineCursor (screen->display->xdisplay, screen->xroot, xcursor);
  XFlush (screen->display->xdisplay);
  XFreeCursor (screen->display->xdisplay, xcursor);
}

#define MAX_PREVIEW_SIZE 150.0

static GdkPixbuf *
get_window_pixbuf (MetaWindow *window,
                   int        *width,
                   int        *height)
{
  cairo_surface_t *surface;
  GdkPixbuf *pixbuf, *scaled;
  double ratio;

  surface = meta_compositor_get_window_surface (window->display->compositor,
                                            window);
  if (surface == NULL)
    return NULL;

  meta_error_trap_push (NULL);

  pixbuf = meta_ui_get_pixbuf_from_surface (surface);
  cairo_surface_destroy (surface);

  if (meta_error_trap_pop_with_return (NULL, FALSE) != Success)
    g_clear_object (&pixbuf);

  if (pixbuf == NULL) 
    return NULL;

  *width = gdk_pixbuf_get_width (pixbuf);
  *height = gdk_pixbuf_get_height (pixbuf);

  /* Scale pixbuf to max dimension MAX_PREVIEW_SIZE */
  if (*width > *height)
    {
      ratio = ((double) *width) / MAX_PREVIEW_SIZE;
      *width = (int) MAX_PREVIEW_SIZE;
      *height = (int) (((double) *height) / ratio);
    }
  else
    {
      ratio = ((double) *height) / MAX_PREVIEW_SIZE;
      *height = (int) MAX_PREVIEW_SIZE;
      *width = (int) (((double) *width) / ratio);
    }

  scaled = gdk_pixbuf_scale_simple (pixbuf, *width, *height,
                                    GDK_INTERP_BILINEAR);
  g_object_unref (pixbuf);
  return scaled;
}
                                         
void
meta_screen_ensure_tab_popup (MetaScreen      *screen,
                              MetaTabList      list_type,
                              MetaTabShowType  show_type)
{
  MetaTabEntry *entries;
  GList *tab_list;
  GList *tmp;
  int len;
  int i;

  if (screen->tab_popup)
    return;

  tab_list = meta_display_get_tab_list (screen->display,
                                        list_type,
                                        screen,
                                        screen->active_workspace);

  len = g_list_length (tab_list);

  entries = g_new (MetaTabEntry, len + 1);
  entries[len].key = NULL;
  entries[len].title = NULL;
  entries[len].icon = NULL;

  i = 0;
  tmp = tab_list;
  while (i < len)
    {
      MetaWindow *window;
      MetaRectangle r;

      window = tmp->data;
      
      entries[i].key = (MetaTabEntryKey) window->xwindow;
      entries[i].title = window->title;

      entries[i].blank = FALSE;
      entries[i].hidden = !meta_window_showing_on_its_workspace (window);
      entries[i].demands_attention = window->wm_state_demands_attention;

      if (show_type == META_TAB_SHOW_INSTANTLY ||
          !entries[i].hidden                   ||
          !meta_window_get_icon_geometry (window, &r))
        meta_window_get_outer_rect (window, &r);

      entries[i].rect = r;

      /* Find inside of highlight rectangle to be used when window is
       * outlined for tabbing.  This should be the size of the
       * east/west frame, and the size of the south frame, on those
       * sides.  On the top it should be the size of the south frame
       * edge.
       */
#define OUTLINE_WIDTH 5
      /* Top side */
      entries[i].inner_rect.y = OUTLINE_WIDTH;

      /* Bottom side */
      entries[i].inner_rect.height = r.height - entries[i].inner_rect.y - OUTLINE_WIDTH;

      /* Left side */
      entries[i].inner_rect.x = OUTLINE_WIDTH;

      /* Right side */
        entries[i].inner_rect.width = r.width - entries[i].inner_rect.x - OUTLINE_WIDTH;

      ++i;
      tmp = tmp->next;
    }

  screen->tab_popup = deepin_tab_popup_new (entries, screen->number, len,
          list_type != META_TAB_LIST_GROUP);

  g_free (entries);

  g_list_free (tab_list);

  /* don't show tab popup, since proper window isn't selected yet */
}

void 
meta_screen_ensure_workspace_indicator (MetaScreen* screen)
{
  if (screen->workspace_indicator) 
    return;

  screen->workspace_indicator = deepin_workspace_indicator_new (screen);
}

void 
meta_screen_ensure_previewing_workspace (MetaScreen* screen)
{
  if (screen->ws_previewer) 
    return;

  screen->ws_previewer = deepin_wm_background_new (screen);
}

void meta_screen_ensure_exposing_windows (MetaScreen* screen)
{
  if (screen->exposing_windows_popup)
    return;

  screen->exposing_windows_popup = gtk_window_new(GTK_WINDOW_POPUP);
  gtk_widget_set_app_paintable(GTK_WIDGET(screen->exposing_windows_popup), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(screen->exposing_windows_popup), 
          screen->rect.width, screen->rect.height);
}

static gboolean
meta_screen_tile_preview_update_timeout (gpointer data)
{
  MetaScreen *screen = data;
  MetaWindow *window = screen->display->grab_window;
  gboolean composited = screen->display->compositor != NULL;
  gboolean needs_preview = FALSE;

  screen->tile_preview_timeout_id = 0;

  if (!screen->tile_preview)
    screen->tile_preview = meta_tile_preview_new (screen->number,
                                                  composited);

  if (window)
    {
      switch (window->tile_mode)
        {
          case META_TILE_LEFT:
          case META_TILE_RIGHT:
              if (!META_WINDOW_TILED_SIDE_BY_SIDE (window))
                needs_preview = TRUE;
              break;

          case META_TILE_MAXIMIZED:
              if (!META_WINDOW_MAXIMIZED (window))
                needs_preview = TRUE;
              break;

          default:
              needs_preview = FALSE;
              break;
        }
    }

  if (needs_preview)
    {
      MetaRectangle tile_rect;

      meta_window_get_current_tile_area (window, &tile_rect);
      meta_tile_preview_show (screen->tile_preview, &tile_rect);
    }
  else
    meta_tile_preview_hide (screen->tile_preview);

  return FALSE;
}

#define TILE_PREVIEW_TIMEOUT_MS 200

void
meta_screen_tile_preview_update (MetaScreen *screen,
                                 gboolean delay)
{
  if (delay)
    {
      if (screen->tile_preview_timeout_id > 0)
        return;

      screen->tile_preview_timeout_id =
        g_timeout_add (TILE_PREVIEW_TIMEOUT_MS,
                       meta_screen_tile_preview_update_timeout,
                       screen);
    }
  else
    {
      if (screen->tile_preview_timeout_id > 0)
        g_source_remove (screen->tile_preview_timeout_id);

      meta_screen_tile_preview_update_timeout ((gpointer)screen);
    }
}

void
meta_screen_tile_preview_hide (MetaScreen *screen)
{
  if (screen->tile_preview_timeout_id > 0)
    g_source_remove (screen->tile_preview_timeout_id);

  if (screen->tile_preview)
    meta_tile_preview_hide (screen->tile_preview);
}

MetaWindow*
meta_screen_get_mouse_window (MetaScreen  *screen,
                              MetaWindow  *not_this_one)
{
  MetaWindow *window;
  Window root_return, child_return;
  int root_x_return, root_y_return;
  int win_x_return, win_y_return;
  unsigned int mask_return;

  if (not_this_one)
    meta_topic (META_DEBUG_FOCUS,
                "Focusing mouse window excluding %s\n", not_this_one->desc);

  meta_error_trap_push (screen->display);
  XQueryPointer (screen->display->xdisplay,
                 screen->xroot,
                 &root_return,
                 &child_return,
                 &root_x_return,
                 &root_y_return,
                 &win_x_return,
                 &win_y_return,
                 &mask_return);
  meta_error_trap_pop (screen->display, TRUE);

  window = meta_stack_get_default_focus_window_at_point (screen->stack,
                                                         screen->active_workspace,
                                                         not_this_one,
                                                         root_x_return,
                                                         root_y_return);

  return window;
}

const MetaXineramaScreenInfo*
meta_screen_get_xinerama_for_rect (MetaScreen    *screen,
				   MetaRectangle *rect)
{
  int i;
  int best_xinerama, xinerama_score;

  if (screen->n_xinerama_infos == 1)
    return &screen->xinerama_infos[0];

  best_xinerama = 0;
  xinerama_score = 0;

  for (i = 0; i < screen->n_xinerama_infos; i++)
    {
      MetaRectangle dest;
      if (meta_rectangle_intersect (&screen->xinerama_infos[i].rect,
                                    rect,
                                    &dest))
        {
          int cur = meta_rectangle_area (&dest);
          if (cur > xinerama_score)
            {
              xinerama_score = cur;
              best_xinerama = i;
            }
        }
    }

  return &screen->xinerama_infos[best_xinerama];
}

const MetaXineramaScreenInfo*
meta_screen_get_xinerama_for_window (MetaScreen *screen,
				     MetaWindow *window)
{
  MetaRectangle window_rect;

  meta_window_get_outer_rect (window, &window_rect);

  return meta_screen_get_xinerama_for_rect (screen, &window_rect);
}

const MetaXineramaScreenInfo*
meta_screen_get_xinerama_neighbor (MetaScreen         *screen,
                                   int                 which_xinerama,
                                   MetaScreenDirection direction)
{
  MetaXineramaScreenInfo* input = screen->xinerama_infos + which_xinerama;
  MetaXineramaScreenInfo* current;
  int i;

  for (i = 0; i < screen->n_xinerama_infos; i++)
    {
      current = screen->xinerama_infos + i;

      if ((direction == META_SCREEN_RIGHT &&
           current->rect.x == input->rect.x + input->rect.width &&
           meta_rectangle_vert_overlap(&current->rect, &input->rect)) ||
          (direction == META_SCREEN_LEFT &&
           input->rect.x == current->rect.x + current->rect.width &&
           meta_rectangle_vert_overlap(&current->rect, &input->rect)) ||
          (direction == META_SCREEN_UP &&
           input->rect.y == current->rect.y + current->rect.height &&
           meta_rectangle_horiz_overlap(&current->rect, &input->rect)) ||
          (direction == META_SCREEN_DOWN &&
           current->rect.y == input->rect.y + input->rect.height &&
           meta_rectangle_horiz_overlap(&current->rect, &input->rect)))
        {
          return current;
        }
    }

  return NULL;
}

void
meta_screen_get_natural_xinerama_list (MetaScreen *screen,
                                       int**       xineramas_list,
                                       int*        n_xineramas)
{
  const MetaXineramaScreenInfo* current;
  const MetaXineramaScreenInfo* tmp;
  GQueue* xinerama_queue;
  int* visited;
  int cur = 0;
  int i;

  *n_xineramas = screen->n_xinerama_infos;
  *xineramas_list = g_new (int, screen->n_xinerama_infos);

  /* we calculate a natural ordering by which to choose xineramas for
   * window placement.  We start at the current xinerama, and perform
   * a breadth-first search of the xineramas starting from that
   * xinerama.  We choose preferentially left, then right, then down,
   * then up.  The visitation order produced by this traversal is the
   * natural xinerama ordering.
   */

  visited = g_new (int, screen->n_xinerama_infos);
  for (i = 0; i < screen->n_xinerama_infos; i++)
    {
      visited[i] = FALSE;
    }

  current = meta_screen_get_current_xinerama (screen);
  xinerama_queue = g_queue_new ();
  g_queue_push_tail (xinerama_queue, (gpointer) current);
  visited[current->number] = TRUE;

  while (!g_queue_is_empty (xinerama_queue))
    {
      current = (const MetaXineramaScreenInfo*)
        g_queue_pop_head (xinerama_queue);

      (*xineramas_list)[cur++] = current->number;

      /* enqueue each of the directions */
      tmp = meta_screen_get_xinerama_neighbor (screen,
                                               current->number,
                                               META_SCREEN_LEFT);
      if (tmp && !visited[tmp->number])
        {
          g_queue_push_tail (xinerama_queue,
                             (MetaXineramaScreenInfo*) tmp);
          visited[tmp->number] = TRUE;
        }
      tmp = meta_screen_get_xinerama_neighbor (screen,
                                               current->number,
                                               META_SCREEN_RIGHT);
      if (tmp && !visited[tmp->number])
        {
          g_queue_push_tail (xinerama_queue,
                             (MetaXineramaScreenInfo*) tmp);
          visited[tmp->number] = TRUE;
        }
      tmp = meta_screen_get_xinerama_neighbor (screen,
                                               current->number,
                                               META_SCREEN_UP);
      if (tmp && !visited[tmp->number])
        {
          g_queue_push_tail (xinerama_queue,
                             (MetaXineramaScreenInfo*) tmp);
          visited[tmp->number] = TRUE;
        }
      tmp = meta_screen_get_xinerama_neighbor (screen,
                                               current->number,
                                               META_SCREEN_DOWN);
      if (tmp && !visited[tmp->number])
        {
          g_queue_push_tail (xinerama_queue,
                             (MetaXineramaScreenInfo*) tmp);
          visited[tmp->number] = TRUE;
        }
    }

  /* in case we somehow missed some set of xineramas, go through the
   * visited list and add in any xineramas that were missed
   */
  for (i = 0; i < screen->n_xinerama_infos; i++)
    {
      if (visited[i] == FALSE)
        {
          (*xineramas_list)[cur++] = i;
        }
    }

  g_free (visited);
  g_queue_free (xinerama_queue);
}

const MetaXineramaScreenInfo*
meta_screen_get_current_xinerama (MetaScreen *screen)
{
  if (screen->n_xinerama_infos == 1)
    return &screen->xinerama_infos[0];

  /* Sadly, we have to do it this way. Yuck.
   */

  if (screen->display->xinerama_cache_invalidated)
    {
      Window root_return, child_return;
      int win_x_return, win_y_return;
      unsigned int mask_return;
      int i;
      MetaRectangle pointer_position;

      screen->display->xinerama_cache_invalidated = FALSE;

      pointer_position.width = pointer_position.height = 1;
      XQueryPointer (screen->display->xdisplay,
                     screen->xroot,
                     &root_return,
                     &child_return,
                     &pointer_position.x,
                     &pointer_position.y,
                     &win_x_return,
                     &win_y_return,
                     &mask_return);

      screen->last_xinerama_index = 0;
      for (i = 0; i < screen->n_xinerama_infos; i++)
        {
          if (meta_rectangle_contains_rect (&screen->xinerama_infos[i].rect,
                                            &pointer_position))
            {
              screen->last_xinerama_index = i;
              break;
            }
        }

      meta_topic (META_DEBUG_XINERAMA,
                  "Rechecked current Xinerama, now %d\n",
                  screen->last_xinerama_index);
    }

  return &screen->xinerama_infos[screen->last_xinerama_index];
}

#define _NET_WM_ORIENTATION_HORZ 0
#define _NET_WM_ORIENTATION_VERT 1

#define _NET_WM_TOPLEFT     0
#define _NET_WM_TOPRIGHT    1
#define _NET_WM_BOTTOMRIGHT 2
#define _NET_WM_BOTTOMLEFT  3

void
meta_screen_update_workspace_layout (MetaScreen *screen)
{
  gulong *list;
  int n_items;

  list = NULL;
  n_items = 0;

  if (meta_prop_get_cardinal_list (screen->display,
                                   screen->xroot,
                                   screen->display->atom__NET_DESKTOP_LAYOUT,
                                   &list, &n_items))
    {
      if (n_items == 3 || n_items == 4)
        {
          int cols, rows;

          switch (list[0])
            {
            case _NET_WM_ORIENTATION_HORZ:
              screen->vertical_workspaces = FALSE;
              break;
            case _NET_WM_ORIENTATION_VERT:
              screen->vertical_workspaces = TRUE;
              break;
            default:
              meta_warning ("Someone set a weird orientation in _NET_DESKTOP_LAYOUT\n");
              break;
            }

          cols = list[1];
          rows = list[2];

          if (rows <= 0 && cols <= 0)
            {
              meta_warning ("Columns = %d rows = %d in _NET_DESKTOP_LAYOUT makes no sense\n", rows, cols);
            }
          else
            {
              if (rows > 0)
                screen->rows_of_workspaces = rows;
              else
                screen->rows_of_workspaces = -1;

              if (cols > 0)
                screen->columns_of_workspaces = cols;
              else
                screen->columns_of_workspaces = -1;
            }

          if (n_items == 4)
            {
              switch (list[3])
                {
                  case _NET_WM_TOPLEFT:
                    screen->starting_corner = META_SCREEN_TOPLEFT;
                    break;
                  case _NET_WM_TOPRIGHT:
                    screen->starting_corner = META_SCREEN_TOPRIGHT;
                    break;
                  case _NET_WM_BOTTOMRIGHT:
                    screen->starting_corner = META_SCREEN_BOTTOMRIGHT;
                    break;
                  case _NET_WM_BOTTOMLEFT:
                    screen->starting_corner = META_SCREEN_BOTTOMLEFT;
                    break;
                  default:
                    meta_warning ("Someone set a weird starting corner in _NET_DESKTOP_LAYOUT\n");
                    break;
                }
            }
          else
            screen->starting_corner = META_SCREEN_TOPLEFT;
        }
      else
        {
          meta_warning ("Someone set _NET_DESKTOP_LAYOUT to %d integers instead of 4 "
                        "(3 is accepted for backwards compat)\n", n_items);
        }

      meta_XFree (list);
    }

  meta_verbose ("Workspace layout rows = %d cols = %d orientation = %d starting corner = %u\n",
                screen->rows_of_workspaces,
                screen->columns_of_workspaces,
                screen->vertical_workspaces,
                screen->starting_corner);
}

static void
set_workspace_names (MetaScreen *screen)
{
  /* This updates names on root window when the pref changes,
   * note we only get prefs change notify if things have
   * really changed.
   */
  GString *flattened;
  int i;
  int n_spaces;

  /* flatten to nul-separated list */
  n_spaces = meta_screen_get_n_workspaces (screen);
  flattened = g_string_new ("");
  i = 0;
  while (i < n_spaces)
    {
      const char *name;

      name = meta_prefs_get_workspace_name (i);

      if (name)
        g_string_append_len (flattened, name,
                             strlen (name) + 1);
      else
        g_string_append_len (flattened, "", 1);

      ++i;
    }

  meta_error_trap_push (screen->display);
  XChangeProperty (screen->display->xdisplay,
                   screen->xroot,
                   screen->display->atom__NET_DESKTOP_NAMES,
		   screen->display->atom_UTF8_STRING,
                   8, PropModeReplace,
		   (unsigned char *)flattened->str, flattened->len);
  meta_error_trap_pop (screen->display, FALSE);

  g_string_free (flattened, TRUE);
}

void
meta_screen_update_workspace_names (MetaScreen *screen)
{
  char **names;
  int n_names;
  int i;

  /* this updates names in prefs when the root window property changes,
   * iff the new property contents don't match what's already in prefs
   */

  names = NULL;
  n_names = 0;
  if (!meta_prop_get_utf8_list (screen->display,
                                screen->xroot,
                                screen->display->atom__NET_DESKTOP_NAMES,
                                &names, &n_names))
    {
      meta_verbose ("Failed to get workspace names from root window %d\n",
                    screen->number);
      return;
    }

  i = 0;
  while (i < n_names)
    {
      meta_topic (META_DEBUG_PREFS,
                  "Setting workspace %d name to \"%s\" due to _NET_DESKTOP_NAMES change\n",
                  i, names[i] ? names[i] : "null");
      meta_prefs_change_workspace_name (i, names[i]);

      ++i;
    }

  g_strfreev (names);
}

Window
meta_create_offscreen_window (Display *xdisplay,
                              Window   parent,
                              long     valuemask)
{
  XSetWindowAttributes attrs;

  /* we want to be override redirect because sometimes we
   * create a window on a screen we aren't managing.
   * (but on a display we are managing at least one screen for)
   */
  attrs.override_redirect = True;
  attrs.event_mask = valuemask;

  return XCreateWindow (xdisplay,
                        parent,
                        -100, -100, 1, 1,
                        0,
                        CopyFromParent,
                        CopyFromParent,
                        (Visual *)CopyFromParent,
                        CWOverrideRedirect | CWEventMask,
                        &attrs);
}

static void
set_work_area_hint (MetaScreen *screen)
{
  int num_workspaces;
  GList *tmp_list;
  unsigned long *data, *tmp;
  MetaRectangle area;

  num_workspaces = meta_screen_get_n_workspaces (screen);
  data = g_new (unsigned long, num_workspaces * 4);
  tmp_list = screen->workspaces;
  tmp = data;

  while (tmp_list != NULL)
    {
      MetaWorkspace *workspace = tmp_list->data;

      if (workspace->screen == screen)
        {
          meta_workspace_get_work_area_all_xineramas (workspace, &area);
          tmp[0] = area.x;
          tmp[1] = area.y;
          tmp[2] = area.width;
          tmp[3] = area.height;

	  tmp += 4;
        }

      tmp_list = tmp_list->next;
    }

  meta_error_trap_push (screen->display);
  XChangeProperty (screen->display->xdisplay, screen->xroot,
		   screen->display->atom__NET_WORKAREA,
		   XA_CARDINAL, 32, PropModeReplace,
		   (guchar*) data, num_workspaces*4);
  g_free (data);
  meta_error_trap_pop (screen->display, FALSE);
}

static gboolean
set_work_area_idle_func (MetaScreen *screen)
{
  meta_topic (META_DEBUG_WORKAREA,
              "Running work area idle function\n");

  screen->work_area_idle = 0;

  set_work_area_hint (screen);

  return FALSE;
}

void
meta_screen_queue_workarea_recalc (MetaScreen *screen)
{
  /* Recompute work area in an idle */
  if (screen->work_area_idle == 0)
    {
      meta_topic (META_DEBUG_WORKAREA,
                  "Adding work area hint idle function\n");
      screen->work_area_idle =
        g_idle_add_full (META_PRIORITY_WORK_AREA_HINT,
                         (GSourceFunc) set_work_area_idle_func,
                         screen,
                         NULL);
    }
}


#ifdef WITH_VERBOSE_MODE
static const gchar *
meta_screen_corner_to_string (MetaScreenCorner corner)
{
  switch (corner)
    {
    case META_SCREEN_TOPLEFT:
      return "TopLeft";
    case META_SCREEN_TOPRIGHT:
      return "TopRight";
    case META_SCREEN_BOTTOMLEFT:
      return "BottomLeft";
    case META_SCREEN_BOTTOMRIGHT:
      return "BottomRight";
    default:
      break;
    }

  return "Unknown";
}
#endif /* WITH_VERBOSE_MODE */

void
meta_screen_calc_workspace_layout (MetaScreen          *screen,
                                   int                  num_workspaces,
                                   int                  current_space,
                                   MetaWorkspaceLayout *layout)
{
  int rows, cols;
  int grid_area;
  int *grid;
  int i, r, c;
  int current_row, current_col;

  rows = screen->rows_of_workspaces;
  cols = screen->columns_of_workspaces;
  if (rows <= 0 && cols <= 0)
    cols = num_workspaces;

  if (rows <= 0)
    rows = num_workspaces / cols + ((num_workspaces % cols) > 0 ? 1 : 0);
  if (cols <= 0)
    cols = num_workspaces / rows + ((num_workspaces % rows) > 0 ? 1 : 0);

  /* paranoia */
  if (rows < 1)
    rows = 1;
  if (cols < 1)
    cols = 1;

  g_assert (rows != 0 && cols != 0);

  grid_area = rows * cols;

  meta_verbose ("Getting layout rows = %d cols = %d current = %d "
                "num_spaces = %d vertical = %s corner = %s\n",
                rows, cols, current_space, num_workspaces,
                screen->vertical_workspaces ? "(true)" : "(false)",
                meta_screen_corner_to_string (screen->starting_corner));

  /* ok, we want to setup the distances in the workspace array to go
   * in each direction. Remember, there are many ways that a workspace
   * array can be setup.
   * see http://www.freedesktop.org/standards/wm-spec/1.2/html/x109.html
   * and look at the _NET_DESKTOP_LAYOUT section for details.
   * For instance:
   */
  /* starting_corner = META_SCREEN_TOPLEFT
   *  vertical_workspaces = 0                 vertical_workspaces=1
   *       1234                                    1357
   *       5678                                    2468
   *
   * starting_corner = META_SCREEN_TOPRIGHT
   *  vertical_workspaces = 0                 vertical_workspaces=1
   *       4321                                    7531
   *       8765                                    8642
   *
   * starting_corner = META_SCREEN_BOTTOMLEFT
   *  vertical_workspaces = 0                 vertical_workspaces=1
   *       5678                                    2468
   *       1234                                    1357
   *
   * starting_corner = META_SCREEN_BOTTOMRIGHT
   *  vertical_workspaces = 0                 vertical_workspaces=1
   *       8765                                    8642
   *       4321                                    7531
   *
   */
  /* keep in mind that we could have a ragged layout, e.g. the "8"
   * in the above grids could be missing
   */


  grid = g_new (int, grid_area);

  current_row = -1;
  current_col = -1;
  i = 0;

  switch (screen->starting_corner)
    {
    case META_SCREEN_TOPLEFT:
      if (screen->vertical_workspaces)
        {
          c = 0;
          while (c < cols)
            {
              r = 0;
              while (r < rows)
                {
                  grid[r*cols+c] = i;
                  ++i;
                  ++r;
                }
              ++c;
            }
        }
      else
        {
          r = 0;
          while (r < rows)
            {
              c = 0;
              while (c < cols)
                {
                  grid[r*cols+c] = i;
                  ++i;
                  ++c;
                }
              ++r;
            }
        }
      break;
    case META_SCREEN_TOPRIGHT:
      if (screen->vertical_workspaces)
        {
          c = cols - 1;
          while (c >= 0)
            {
              r = 0;
              while (r < rows)
                {
                  grid[r*cols+c] = i;
                  ++i;
                  ++r;
                }
              --c;
            }
        }
      else
        {
          r = 0;
          while (r < rows)
            {
              c = cols - 1;
              while (c >= 0)
                {
                  grid[r*cols+c] = i;
                  ++i;
                  --c;
                }
              ++r;
            }
        }
      break;
    case META_SCREEN_BOTTOMLEFT:
      if (screen->vertical_workspaces)
        {
          c = 0;
          while (c < cols)
            {
              r = rows - 1;
              while (r >= 0)
                {
                  grid[r*cols+c] = i;
                  ++i;
                  --r;
                }
              ++c;
            }
        }
      else
        {
          r = rows - 1;
          while (r >= 0)
            {
              c = 0;
              while (c < cols)
                {
                  grid[r*cols+c] = i;
                  ++i;
                  ++c;
                }
              --r;
            }
        }
      break;
    case META_SCREEN_BOTTOMRIGHT:
      if (screen->vertical_workspaces)
        {
          c = cols - 1;
          while (c >= 0)
            {
              r = rows - 1;
              while (r >= 0)
                {
                  grid[r*cols+c] = i;
                  ++i;
                  --r;
                }
              --c;
            }
        }
      else
        {
          r = rows - 1;
          while (r >= 0)
            {
              c = cols - 1;
              while (c >= 0)
                {
                  grid[r*cols+c] = i;
                  ++i;
                  --c;
                }
              --r;
            }
        }
      break;
    default:
      break;
    }

  if (i != grid_area)
    meta_bug ("did not fill in the whole workspace grid in %s (%d filled)\n",
              G_STRFUNC, i);

  current_row = 0;
  current_col = 0;
  r = 0;
  while (r < rows)
    {
      c = 0;
      while (c < cols)
        {
          if (grid[r*cols+c] == current_space)
            {
              current_row = r;
              current_col = c;
            }
          else if (grid[r*cols+c] >= num_workspaces)
            {
              /* flag nonexistent spaces with -1 */
              grid[r*cols+c] = -1;
            }
          ++c;
        }
      ++r;
    }

  layout->rows = rows;
  layout->cols = cols;
  layout->grid = grid;
  layout->grid_area = grid_area;
  layout->current_row = current_row;
  layout->current_col = current_col;

#ifdef WITH_VERBOSE_MODE
  if (meta_is_verbose ())
    {
      r = 0;
      while (r < layout->rows)
        {
          meta_verbose (" ");
          meta_push_no_msg_prefix ();
          c = 0;
          while (c < layout->cols)
            {
              if (r == layout->current_row &&
                  c == layout->current_col)
                meta_verbose ("*%2d ", layout->grid[r*layout->cols+c]);
              else
                meta_verbose ("%3d ", layout->grid[r*layout->cols+c]);
              ++c;
            }
          meta_verbose ("\n");
          meta_pop_no_msg_prefix ();
          ++r;
        }
    }
#endif /* WITH_VERBOSE_MODE */
}

void
meta_screen_free_workspace_layout (MetaWorkspaceLayout *layout)
{
  g_free (layout->grid);
}

static void
meta_screen_resize_func (MetaScreen *screen,
                         MetaWindow *window,
                         void       *user_data)
{
  if (window->struts)
    {
      meta_window_update_struts (window);
    }
  meta_window_queue (window, META_QUEUE_MOVE_RESIZE);

  meta_window_recalc_features (window);
}

static void
on_screen_scaled(DeepinMessageHub* hub, gdouble scale,
        gpointer data)
{
  MetaScreen *screen = (MetaScreen*)data;

  if (screen->corner_windows[0] != None) 
    {
      int i;
      MetaScreenCorner corners[] = {
          META_SCREEN_TOPLEFT,
          META_SCREEN_TOPRIGHT,
          META_SCREEN_BOTTOMLEFT,
          META_SCREEN_BOTTOMRIGHT,
      };

      int CORNER_SIZE = CORNER_BASE_SIZE * deepin_message_hub_get_screen_scale ();
      int positions[8];
      meta_screen_calc_corner_positions (screen, positions);

      for (i = 0; i < 4; i++) 
        {
          int x = positions[i * 2];
          int y = positions[i * 2 + 1];

          GdkWindow *window = gtk_widget_get_window (screen->corner_indicator[i]);
          gdk_window_move_resize (window, x, y, CORNER_SIZE, CORNER_SIZE);
          gdk_window_raise (window);
        }

      for (i = 0; i < 4; i++) 
        {
          XWindowChanges changes;

          changes.x = positions[i * 2];
          changes.y = positions[i * 2 + 1];
          changes.width = CORNER_SIZE;
          changes.height = CORNER_SIZE;

          XConfigureWindow(screen->display->xdisplay, screen->corner_windows[i],
                  CWX | CWY | CWWidth | CWHeight, &changes);
        }
    }
}

void
meta_screen_update_corner (MetaScreen* screen, CornerUpdateMask update)
{
  if (screen->corner_windows[0] != None) 
    {
      int i;
      MetaScreenCorner corners[] = {
          META_SCREEN_TOPLEFT,
          META_SCREEN_TOPRIGHT,
          META_SCREEN_BOTTOMLEFT,
          META_SCREEN_BOTTOMRIGHT,
      };

      int positions[8];
      if (update & CORNER_UPDATE_POS)
        {
          meta_screen_calc_corner_positions (screen, positions);
        }

      // re-arrange all corner related windows
      Window windows[8];
      for (i = 0; i < 4; i++)
        {
          windows[i*2] = GDK_WINDOW_XID(gtk_widget_get_window(screen->corner_indicator[i]));
          windows[i*2+1] = screen->corner_windows[i];
        }

      for (i = 0; i < 8; i++) 
        {
          XWindowChanges changes;

          if (update & CORNER_UPDATE_POS)
            {
              changes.x = positions[(i/2) * 2];
              changes.y = positions[(i/2) * 2 + 1];
            }

          unsigned mask = 0;
          if (update & CORNER_UPDATE_POS)
              mask |= CWX | CWY;

          if (update & CORNER_UPDATE_STACK)
              mask |= CWStackMode;

          if (i == 0) {
              if (update & CORNER_UPDATE_STACK)
                changes.stack_mode = Above;
              XConfigureWindow(screen->display->xdisplay, windows[i],
                      mask, &changes);
          } else {
              if (update & CORNER_UPDATE_STACK)
                {
                  changes.stack_mode = Below;
                  mask |= CWSibling;
                }
              changes.sibling = windows[i-1];
              XConfigureWindow(screen->display->xdisplay, windows[i],
                      mask, &changes);
          }
        }
    }
}

static void
on_screen_changed(DeepinMessageHub* hub, MetaScreen* screen,
        gpointer data)
{
  GdkScreen* gdkscreen = gdk_screen_get_default(); 
  screen->rect.width = gdk_screen_get_width(gdkscreen);
  screen->rect.height = gdk_screen_get_height(gdkscreen);

  /* Resize the guard window to fill the screen again. */
  if (screen->guard_window != None)
    {
      XWindowChanges changes;

      changes.x = 0;
      changes.y = 0;
      changes.width = screen->rect.width;
      changes.height = screen->rect.height;

      XConfigureWindow(screen->display->xdisplay,
                       screen->guard_window,
                       CWX | CWY | CWWidth | CWHeight,
                       &changes);
    }

  meta_screen_update_corner (screen, CORNER_UPDATE_POS | CORNER_UPDATE_STACK);

  meta_screen_rebuild_backgrounds (screen);

  reload_xinerama_infos (screen);
  set_desktop_geometry_hint (screen);

  /* Queue a resize on all the windows */
  meta_screen_foreach_window (screen, meta_screen_resize_func, 0);
}

void
meta_screen_update_showing_desktop_hint (MetaScreen *screen)
{
  unsigned long data[1];

  data[0] = screen->active_workspace->showing_desktop ? 1 : 0;

  meta_error_trap_push (screen->display);
  XChangeProperty (screen->display->xdisplay, screen->xroot,
                   screen->display->atom__NET_SHOWING_DESKTOP,
                   XA_CARDINAL,
                   32, PropModeReplace, (guchar*) data, 1);
  meta_error_trap_pop (screen->display, FALSE);
}

static void
queue_windows_showing (MetaScreen *screen)
{
  GSList *windows;
  GSList *tmp;

  /* Must operate on all windows on display instead of just on the
   * active_workspace's window list, because the active_workspace's
   * window list may not contain the on_all_workspace windows.
   */
  windows = meta_display_list_windows (screen->display);

  tmp = windows;
  while (tmp != NULL)
    {
      MetaWindow *w = tmp->data;

      if (w->screen == screen)
        meta_window_queue (w, META_QUEUE_CALC_SHOWING);

      tmp = tmp->next;
    }

  g_slist_free (windows);
}

void
meta_screen_minimize_all_on_active_workspace_except (MetaScreen *screen,
                                                     MetaWindow *keep)
{
  GList *windows;
  GList *tmp;

  windows = screen->active_workspace->windows;

  tmp = windows;
  while (tmp != NULL)
    {
      MetaWindow *w = tmp->data;

      if (w->screen == screen  &&
          w->has_minimize_func &&
	  w != keep)
	meta_window_minimize (w);

      tmp = tmp->next;
    }
}

void
meta_screen_show_desktop (MetaScreen *screen,
                          guint32     timestamp)
{
  GList *windows;

  if (screen->active_workspace->showing_desktop)
    return;

  screen->active_workspace->showing_desktop = TRUE;

  queue_windows_showing (screen);

  /* Focus the most recently used META_WINDOW_DESKTOP window, if there is one;
   * see bug 159257.
   */
  windows = screen->active_workspace->mru_list;
  while (windows != NULL)
    {
      MetaWindow *w = windows->data;

      if (w->screen == screen  &&
          w->type == META_WINDOW_DESKTOP)
        {
          meta_window_focus (w, timestamp);
          break;
        }

      windows = windows->next;
    }


  meta_screen_update_showing_desktop_hint (screen);
}

void
meta_screen_unshow_desktop (MetaScreen *screen)
{
  if (!screen->active_workspace->showing_desktop)
    return;

  screen->active_workspace->showing_desktop = FALSE;

  queue_windows_showing (screen);

  meta_screen_update_showing_desktop_hint (screen);
}


#ifdef HAVE_STARTUP_NOTIFICATION
static gboolean startup_sequence_timeout (void *data);

static void
update_startup_feedback (MetaScreen *screen)
{
  if (screen->startup_sequences != NULL)
    {
      meta_topic (META_DEBUG_STARTUP,
                  "Setting busy cursor\n");
      meta_screen_set_cursor (screen, META_CURSOR_BUSY);
    }
  else
    {
      meta_topic (META_DEBUG_STARTUP,
                  "Setting default cursor\n");
      meta_screen_set_cursor (screen, META_CURSOR_DEFAULT);
    }
}

static void
add_sequence (MetaScreen        *screen,
              SnStartupSequence *sequence)
{
  meta_topic (META_DEBUG_STARTUP,
              "Adding sequence %s\n",
              sn_startup_sequence_get_id (sequence));
  sn_startup_sequence_ref (sequence);
  screen->startup_sequences = g_slist_prepend (screen->startup_sequences,
                                               sequence);

  /* our timeout just polls every second, instead of bothering
   * to compute exactly when we may next time out
   */
  if (screen->startup_sequence_timeout == 0)
    screen->startup_sequence_timeout = g_timeout_add (1000,
                                                      startup_sequence_timeout,
                                                      screen);

  update_startup_feedback (screen);
}

static void
remove_sequence (MetaScreen        *screen,
                 SnStartupSequence *sequence)
{
  meta_topic (META_DEBUG_STARTUP,
              "Removing sequence %s\n",
              sn_startup_sequence_get_id (sequence));

  screen->startup_sequences = g_slist_remove (screen->startup_sequences,
                                              sequence);
  sn_startup_sequence_unref (sequence);

  if (screen->startup_sequences == NULL &&
      screen->startup_sequence_timeout != 0)
    {
      g_source_remove (screen->startup_sequence_timeout);
      screen->startup_sequence_timeout = 0;
    }

  update_startup_feedback (screen);
}

typedef struct
{
  GSList *list;
  GTimeVal now;
} CollectTimedOutData;

/* This should be fairly long, as it should never be required unless
 * apps or .desktop files are buggy, and it's confusing if
 * OpenOffice or whatever seems to stop launching - people
 * might decide they need to launch it again.
 */
#define STARTUP_TIMEOUT 15000

static void
collect_timed_out_foreach (void *element,
                           void *data)
{
  CollectTimedOutData *ctod = data;
  SnStartupSequence *sequence = element;
  long tv_sec, tv_usec;
  double elapsed;

  sn_startup_sequence_get_last_active_time (sequence, &tv_sec, &tv_usec);

  elapsed =
    ((((double)ctod->now.tv_sec - tv_sec) * G_USEC_PER_SEC +
      (ctod->now.tv_usec - tv_usec))) / 1000.0;

  meta_topic (META_DEBUG_STARTUP,
              "Sequence used %g seconds vs. %g max: %s\n",
              elapsed, (double) STARTUP_TIMEOUT,
              sn_startup_sequence_get_id (sequence));

  if (elapsed > STARTUP_TIMEOUT)
    ctod->list = g_slist_prepend (ctod->list, sequence);
}

static gboolean
startup_sequence_timeout (void *data)
{
  MetaScreen *screen = data;
  CollectTimedOutData ctod;
  GSList *tmp;

  ctod.list = NULL;
  g_get_current_time (&ctod.now);
  g_slist_foreach (screen->startup_sequences,
                   collect_timed_out_foreach,
                   &ctod);

  tmp = ctod.list;
  while (tmp != NULL)
    {
      SnStartupSequence *sequence = tmp->data;

      meta_topic (META_DEBUG_STARTUP,
                  "Timed out sequence %s\n",
                  sn_startup_sequence_get_id (sequence));

      sn_startup_sequence_complete (sequence);

      tmp = tmp->next;
    }

  g_slist_free (ctod.list);

  if (screen->startup_sequences != NULL)
    {
      return TRUE;
    }
  else
    {
      /* remove */
      screen->startup_sequence_timeout = 0;
      return FALSE;
    }
}

static void
meta_screen_sn_event (SnMonitorEvent *event,
                      void           *user_data)
{
  MetaScreen *screen;
  SnStartupSequence *sequence;

  screen = user_data;

  sequence = sn_monitor_event_get_startup_sequence (event);

  switch (sn_monitor_event_get_type (event))
    {
    case SN_MONITOR_EVENT_INITIATED:
      {
        const char *wmclass;

        wmclass = sn_startup_sequence_get_wmclass (sequence);

        meta_topic (META_DEBUG_STARTUP,
                    "Received startup initiated for %s wmclass %s\n",
                    sn_startup_sequence_get_id (sequence),
                    wmclass ? wmclass : "(unset)");
        add_sequence (screen, sequence);
      }
      break;

    case SN_MONITOR_EVENT_COMPLETED:
      {
        meta_topic (META_DEBUG_STARTUP,
                    "Received startup completed for %s\n",
                    sn_startup_sequence_get_id (sequence));
        remove_sequence (screen,
                         sn_monitor_event_get_startup_sequence (event));
      }
      break;

    case SN_MONITOR_EVENT_CHANGED:
      meta_topic (META_DEBUG_STARTUP,
                  "Received startup changed for %s\n",
                  sn_startup_sequence_get_id (sequence));
      break;

    case SN_MONITOR_EVENT_CANCELED:
      meta_topic (META_DEBUG_STARTUP,
                  "Received startup canceled for %s\n",
                  sn_startup_sequence_get_id (sequence));
      break;

    default:
      break;
    }
}
#endif

/* Sets the initial_timestamp and initial_workspace properties
 * of a window according to information given us by the
 * startup-notification library.
 *
 * Returns TRUE if startup properties have been applied, and
 * FALSE if they have not (for example, if they had already
 * been applied.)
 */
gboolean
meta_screen_apply_startup_properties (MetaScreen *screen,
                                      MetaWindow *window)
{
#ifdef HAVE_STARTUP_NOTIFICATION
  const char *startup_id;
  GSList *tmp;
  SnStartupSequence *sequence;

  /* Does the window have a startup ID stored? */
  startup_id = meta_window_get_startup_id (window);

  meta_topic (META_DEBUG_STARTUP,
              "Applying startup props to %s id \"%s\"\n",
              window->desc,
              startup_id ? startup_id : "(none)");

  sequence = NULL;
  if (startup_id == NULL)
    {
      /* No startup ID stored for the window. Let's ask the
       * startup-notification library whether there's anything
       * stored for the resource name or resource class hints.
       */
      tmp = screen->startup_sequences;
      while (tmp != NULL)
        {
          const char *wmclass;

          wmclass = sn_startup_sequence_get_wmclass (tmp->data);

          if (wmclass != NULL &&
              ((window->res_class &&
                strcmp (wmclass, window->res_class) == 0) ||
               (window->res_name &&
                strcmp (wmclass, window->res_name) == 0)))
            {
              sequence = tmp->data;

              g_assert (window->startup_id == NULL);
              window->startup_id = g_strdup (sn_startup_sequence_get_id (sequence));
              startup_id = window->startup_id;

              meta_topic (META_DEBUG_STARTUP,
                          "Ending legacy sequence %s due to window %s\n",
                          sn_startup_sequence_get_id (sequence),
                          window->desc);

              sn_startup_sequence_complete (sequence);
              break;
            }

          tmp = tmp->next;
        }
    }

  /* Still no startup ID? Bail. */
  if (startup_id == NULL)
    return FALSE;

  /* We might get this far and not know the sequence ID (if the window
   * already had a startup ID stored), so let's look for one if we don't
   * already know it.
   */
  if (sequence == NULL)
    {
      tmp = screen->startup_sequences;
      while (tmp != NULL)
        {
          const char *id;

          id = sn_startup_sequence_get_id (tmp->data);

          if (strcmp (id, startup_id) == 0)
            {
              sequence = tmp->data;
              break;
            }

          tmp = tmp->next;
        }
    }

  if (sequence != NULL)
    {
      gboolean changed_something = FALSE;

      meta_topic (META_DEBUG_STARTUP,
                  "Found startup sequence for window %s ID \"%s\"\n",
                  window->desc, startup_id);

      if (!window->initial_workspace_set)
        {
          int space = sn_startup_sequence_get_workspace (sequence);
          if (space >= 0)
            {
              meta_topic (META_DEBUG_STARTUP,
                          "Setting initial window workspace to %d based on startup info\n",
                          space);

              window->initial_workspace_set = TRUE;
              window->initial_workspace = space;
              changed_something = TRUE;
            }
        }

      if (!window->initial_timestamp_set)
        {
          guint32 timestamp = sn_startup_sequence_get_timestamp (sequence);
          meta_topic (META_DEBUG_STARTUP,
                      "Setting initial window timestamp to %u based on startup info\n",
                      timestamp);

          window->initial_timestamp_set = TRUE;
          window->initial_timestamp = timestamp;
          changed_something = TRUE;
        }

      return changed_something;
    }
  else
    {
      meta_topic (META_DEBUG_STARTUP,
                  "Did not find startup sequence for window %s ID \"%s\"\n",
                  window->desc, startup_id);
    }

#endif /* HAVE_STARTUP_NOTIFICATION */

  return FALSE;
}

int
meta_screen_get_screen_number (MetaScreen *screen)
{
  return screen->number;
}

MetaDisplay *
meta_screen_get_display (MetaScreen *screen)
{
  return screen->display;
}

Window
meta_screen_get_xroot (MetaScreen *screen)
{
  return screen->xroot;
}

void
meta_screen_get_size (MetaScreen *screen,
                      int        *width,
                      int        *height)
{
  *width = screen->rect.width;
  *height = screen->rect.height;
}

gpointer
meta_screen_get_compositor_data (MetaScreen *screen)
{
  return screen->compositor_data;
}

void
meta_screen_set_compositor_data (MetaScreen *screen,
                                 gpointer    compositor)
{
  screen->compositor_data = compositor;
}

#ifdef HAVE_COMPOSITE_EXTENSIONS
static Window
take_manager_selection (MetaDisplay *display,
                        Atom         manager_atom,
                        Window       xroot,
                        Window       new_owner,
                        int          timestamp,
                        gboolean     should_replace)
{
  Display *xdisplay = display->xdisplay;
  Window current_owner;

  current_owner = XGetSelectionOwner (xdisplay, manager_atom);
  if (current_owner != None)
    {
      XSetWindowAttributes attrs;

      if (should_replace)
        {
          /* We want to find out when the current selection owner dies */
          meta_error_trap_push (display);
          attrs.event_mask = StructureNotifyMask;
          XChangeWindowAttributes (xdisplay, current_owner, CWEventMask, &attrs);
          if (meta_error_trap_pop_with_return (display, FALSE) != Success)
            current_owner = None; /* don't wait for it to die later on */
        }
    }

  /* We need SelectionClear and SelectionRequest events on the new owner,
   * but those cannot be masked, so we only need NoEventMask.
   */
  XSetSelectionOwner (xdisplay, manager_atom, new_owner, timestamp);

  if (XGetSelectionOwner (xdisplay, manager_atom) != new_owner)
    {
      meta_warning ("Could not acquire selection: %s", XGetAtomName (xdisplay, manager_atom));
      abort();
    }

  {
    /* Send client message indicating that we are now the selection owner */
    XClientMessageEvent ev;

    ev.type = ClientMessage;
    ev.window = xroot;
    ev.message_type = display->atom_MANAGER;
    ev.format = 32;
    ev.data.l[0] = timestamp;
    ev.data.l[1] = manager_atom;

    XSendEvent (xdisplay, xroot, False, StructureNotifyMask, (XEvent *) &ev);
  }

  /* Wait for old window manager to go away */
  if (current_owner != None)
    {
      XEvent event;

      /* We sort of block infinitely here which is probably lame. */

      meta_verbose ("Waiting for old window manager to exit\n");
      do {
        XWindowEvent (xdisplay, current_owner, StructureNotifyMask, &event);
        g_usleep(G_USEC_PER_SEC / 10);
      } while (event.type != DestroyNotify);
    }
}

void
meta_screen_set_cm_selection (MetaScreen *screen)
{
  char selection[32];
  Atom a;

  screen->wm_cm_timestamp = meta_display_get_current_time_roundtrip (
                                                               screen->display);

  g_snprintf (selection, sizeof(selection), "_NET_WM_CM_S%d", screen->number);
  meta_verbose ("Setting selection: %s\n", selection);
  a = XInternAtom (screen->display->xdisplay, selection, FALSE);
  Window xroot = meta_screen_get_xroot (screen);
  take_manager_selection (screen->display, a, xroot, screen->wm_cm_selection_window,
          screen->wm_cm_timestamp, TRUE);
}

void
meta_screen_unset_cm_selection (MetaScreen *screen)
{
  char selection[32];
  Atom a;

  g_snprintf (selection, sizeof(selection), "_NET_WM_CM_S%d", screen->number);
  a = XInternAtom (screen->display->xdisplay, selection, FALSE);
  XSetSelectionOwner (screen->display->xdisplay, a,
                      None, screen->wm_cm_timestamp);
}

void
meta_screen_reorder_workspace (MetaScreen    *screen,
        MetaWorkspace *workspace,
        int            new_index)
{
  GList     *l;
  int       index;
  int       from, to;
  int       active_index;

  active_index = meta_workspace_index(screen->active_workspace);
  l = g_list_find (screen->workspaces, workspace);
  if (!l)
    return;

  index = meta_workspace_index (workspace);
  if (new_index == index) 
    return;

  if (new_index < index) {
      from = new_index;
      to = index;
  } else {
      from = index;
      to = new_index;
  }

  screen->workspaces = g_list_remove_link (screen->workspaces, l);
  screen->workspaces = g_list_insert (screen->workspaces, l->data, new_index);

  if (active_index != meta_workspace_index(screen->active_workspace))
    {
      guint32 timestamp =
          meta_display_get_current_time_roundtrip (screen->display);
      meta_workspace_activate (screen->active_workspace, timestamp);
    }

  for (; from <= to; from++) 
    {
      MetaWorkspace *w = g_list_nth_data(screen->workspaces, from);
      meta_workspace_update_window_hints (w);
    }

  meta_screen_queue_workarea_recalc (screen);

  deepin_message_hub_workspace_reordered (index, new_index);
}

void 
meta_screen_remove_workspace (MetaScreen *screen,
        MetaWorkspace *workspace)
{
    GList         *l;
    MetaWorkspace *neighbour = NULL;
    GList         *next = NULL;
    gboolean       active_index_changed;
    int            new_num;
    int            remove_index;


    remove_index = meta_workspace_index (workspace);
    l = screen->workspaces;
    while (l) {
        MetaWorkspace *w = l->data;

        if (w == workspace) {
            if (l->next)
                next = l->next;

            if (l->prev)
                neighbour = l->prev->data;
            else if (l->next)
                neighbour = l->next->data;
            else {
                /* Cannot remove the only workspace! */
                return;
            }

            break;
        }

        l = l->next;
    }

    if (!neighbour)
        return;

    meta_workspace_relocate_windows (workspace, neighbour);

    guint32 timestamp =
        meta_display_get_current_time_roundtrip (screen->display);
    if (workspace == screen->active_workspace)
        meta_workspace_activate (neighbour, timestamp);

    /* This also removes the workspace from the screens list */
    meta_workspace_free (workspace);

    new_num = g_list_length (screen->workspaces);

    set_number_of_spaces_hint (screen, new_num);
    meta_prefs_set_num_workspaces (new_num);

    l = next;
    while (l) {
        MetaWorkspace *w = l->data;

        meta_workspace_update_window_hints (w);

        l = l->next;
    }

    meta_screen_queue_workarea_recalc (screen);
    deepin_message_hub_workspace_removed (remove_index);
}

MetaWorkspace*
meta_screen_new_workspace (MetaScreen   *screen)
{
  int new_num;
  MetaWorkspace *new_ws;

  new_ws = meta_workspace_new (screen);

  new_num = g_list_length(screen->workspaces);

  set_number_of_spaces_hint (screen, new_num);
  meta_prefs_set_num_workspaces (new_num);

  meta_screen_queue_workarea_recalc (screen);

  deepin_message_hub_workspace_added (meta_workspace_index (new_ws));
  return new_ws;
}

void
meta_screen_request_hide_windows(MetaScreen* screen)
{
  GSList *tmp;
  GSList *winlist;

  MetaDisplay* display = screen->display;
  if (display->hiding_windows_mode) {
    meta_verbose ("already in hiding_windows_mode\n");
    return;
  }

  if (screen->active_workspace->showing_desktop)
    return;

  screen->active_workspace->showing_desktop = TRUE;
  display->hiding_windows_mode = TRUE;
  queue_windows_showing (screen);

  guint32 timestamp = meta_display_get_current_time_roundtrip (display);
  meta_display_focus_the_no_focus_window (display, screen, timestamp);


  meta_screen_invalidate_backgrounds(screen, NULL);
}

void
meta_screen_cancel_hide_windows(MetaScreen* screen)
{
  GSList *tmp;
  GSList *winlist;

  if (!screen->display->hiding_windows_mode)
      return;

  screen->active_workspace->showing_desktop = FALSE;
  screen->display->hiding_windows_mode = FALSE;

  queue_windows_showing (screen);

  meta_screen_invalidate_backgrounds(screen, NULL);
}

void meta_screen_invalidate_backgrounds(MetaScreen* screen, MetaRectangle* bounds)
{
    GPtrArray* desktop_bgs = screen->desktop_bgs;
    for (int i = 0, n = desktop_bgs->len; i < n; i++) {
        if (bounds == NULL) {
            gtk_widget_queue_draw(g_ptr_array_index(desktop_bgs, i)); 
        } else {
            gtk_widget_queue_draw_area(g_ptr_array_index(desktop_bgs, i), 
                    bounds->x, bounds->y, bounds->width, bounds->height);
        }
    }
}

void          
meta_screen_enter_corner (MetaScreen *screen, MetaScreenCorner corner)
{
  XUnmapWindow (screen->display->xdisplay, screen->corner_windows[corner]);
  deepin_message_hub_screen_corner_entered (screen, corner);
}

void          
meta_screen_leave_corner (MetaScreen *screen, MetaScreenCorner corner)
{
  XMapWindow (screen->display->xdisplay, screen->corner_windows[corner]);
  deepin_message_hub_screen_corner_leaved (screen, corner);
}


void
meta_screen_enable_corner (MetaScreen *screen, MetaScreenCorner corner, gboolean val)
{
  if (screen->corner_enabled[corner] == val)
    return;

  screen->corner_enabled[corner] = val;
  if (val)
    XMapWindow (screen->display->xdisplay, screen->corner_windows[corner]);
  else
    XUnmapWindow (screen->display->xdisplay, screen->corner_windows[corner]);
}

void          
meta_screen_enable_corner_actions (MetaScreen *screen, gboolean enabled)
{
  if (screen->corner_actions_enabled != enabled) 
    {
      screen->corner_actions_enabled = enabled; 
      int i;
      for (i = 0; i < 4; i++)
        {
          meta_screen_enable_corner (screen, i, enabled);
        }
    }
}

#endif /* HAVE_COMPOSITE_EXTENSIONS */
