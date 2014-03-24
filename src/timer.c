/*
 * timer.c
 * A Totem plugin that adds a programmable timer that will cause totem
 * to exit upon expiry.
 * Note that the timer operates independent of the media state, for
 * example:
 *   - a timer can be started when no media is playing
 *   - a timer that expires when no media is playing will still cause totem
 * to exit
 *   - if the media finishes before the timer expires, the timer continues
 * on and upon timer expiry, totem will exit.
 *   - a timer does not restart or cancel itself when the playing media
 * is started, stopped, paused or changed
 *   - only one timer runs at a one time, configuring a timer while one
 * is already running will cancel the first timer.
 *
 *   Copyright (C) 2013 Christopher A. Doyle
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License along
 *   with this program; if not, write to the Free Software Foundation, Inc.,
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "config.h"

#include <glib/gstdio.h>

#include <totem-plugin.h>
#include "totem-interface.h"

#define TOTEM_TYPE_TIMER_PLUGIN (totem_timer_plugin_get_type())
#define TOTEM_TIMER_PLUGIN(o)   (G_TYPE_CHECK_INSTANCE_CAST ((o), TOTEM_TYPE_TIMER_PLUGIN, TotemTimerPlugin))

#define ACTION_GROUP "TimerActions"
#define ACTION_NAME  "Timer"

/* Adjustable timer constants */
#define TIMER_MIN (1)          /* minimum possible timeout value (in minutes) */
#define TIMER_MAX (999)        /* maximum possible timeout value (in minutes) */
#define TIMER_ADJ_DEFAULT (60) /* default timeout value (in minutes) for adjustable timer */
#define TIMER_CANCEL (0)       /* any value outside of TIMER_MIN..TIMER_MAX will cancel timer */

typedef struct {
  TotemObject    *totem;
  GtkActionGroup *action_group;
  GtkActionEntry *action_entries;
  guint           ui_merge_id;
  GThread        *timer_thread;
} TotemTimerPluginPrivate;

TOTEM_PLUGIN_REGISTER(TOTEM_TYPE_TIMER_PLUGIN, TotemTimerPlugin, totem_timer_plugin)

typedef gint16 TimeType; /* Timeout value in minutes. Normally values are between TIMER_MIN..TIMER_MAX,
                            but values outside this range can be used to signal special cases (e.g. cancel timer). */

/* Structure defining data that is shared between the GUI thread and the timer_function thread. */
typedef struct {
  gboolean new;       /* true indicates new data that timer_function thread hasn't processed yet */
  gboolean terminate; /* true indicates that timer_function thread should terminate/exit */
  TimeType timeout;   /* timeout value (in minutes) to configure timer with (any value outside of TIMER_MIN..TIMER_MAX will cancel a timer) */
} SharedDataType;

/* Data shared between the GUI thread and the timer_function thread. */
static SharedDataType data_shared;
static GMutex         data_mutex;
static GCond          data_cond;

/* Callbacks for timer menu item actions. */
static void totem_timer_plugin_timerCancel    (GtkAction *action, TotemTimerPlugin *pi);
static void totem_timer_plugin_timerAdjustable(GtkAction *action, TotemTimerPlugin *pi);
static void totem_timer_plugin_timerFixed     (GtkAction *action, TotemTimerPlugin *pi);

/* A structure defining information related to a menu item. */
typedef struct {
  gchar *name;  /* name and label for timer's menu item */
} TimerMenuItemType;

/* A list of menu items belonging to Timer menu. */
static TimerMenuItemType timerMenuItems [] = {
  { "Cancel"        }, /* cancel the timer             - must be index 0 (TIMER_IDX_CANCEL) */
  { "Adjustable..." }, /* manually configure the timer - must be index 1 (TIMER_IDX_ADJUST) */
  {  "30m"          }, /* fixed timers start at index 2 (TIMER_IDX_FIXED_START) and must */
  {  "60m"          }, /* have format "%3dm", where %3d is within TIMER_MIN..TIMER_MAX */
  {  "90m"          },
  { "120m"          }
};
/* Indexes into timerMenuItems[].  The following must not contain any gaps. */
#define TIMER_IDX_CANCEL      (0) /* must be index 0 */
#define TIMER_IDX_ADJUST      (1) /* must be index 1 */
#define TIMER_IDX_FIXED_START (2) /* fixed timers start at index 2 */

/* Indexes into action_entries[].  The following must not contain any gaps.
   The number of action entries is one greater than timerMenuItems because the parent (Timer menu)
   to the menu items is also an action entry. */
#define ACTION_IDX_MENU        (0) /* timer menu (parent to menu items) must be index 0 */
#define ACTION_IDX_CANCEL      (1) /* menu item cancel must be index 1 */
#define ACTION_IDX_ADJUST      (2) /* menu item adjust must be index 2 */
#define ACTION_IDX_FIXED_START (3) /* menu items for fixed timers must start at 3 */
#define NUM_ACTION_ENTRIES     (G_N_ELEMENTS(timerMenuItems) +1)


/* Thread implementing the timer. */
static void *
timer_function(TotemObject *totem) {
  gint64 end_time; /* absolute time when we want timer to expire */

  g_mutex_lock(&data_mutex);

  do {
    /* wait until new data arrives */
    while (!data_shared.new) {
      g_cond_wait(&data_cond, &data_mutex);
    }
    /* we have received a signal indicating new data */
    data_shared.new = FALSE;  /* acknowledge the new data */

    while ((!data_shared.terminate) && (data_shared.timeout >=  TIMER_MIN) && (data_shared.timeout <=  TIMER_MAX)) {
      end_time = g_get_monotonic_time() + data_shared.timeout * G_TIME_SPAN_MINUTE;

      while (!data_shared.new) {
        if (!g_cond_wait_until(&data_cond, &data_mutex, end_time)) {
          /* timeout has passed. */
          g_mutex_unlock(&data_mutex);
          totem_action_exit(totem);
          return NULL; /* may not get here */
        }
      }
      /* we have received a signal indicating new data */
      data_shared.new = FALSE;  /* acknowledge the new data */
    }
  } while (!data_shared.terminate);

  /* the signal indicated that we should terminate */
  g_mutex_unlock(&data_mutex);
  g_thread_exit(NULL);
  return NULL; /* will never get here */
}


/* Cancel the timer. */
static void
totem_timer_plugin_timerCancel(GtkAction *action, TotemTimerPlugin *pi) {
  GtkAction *cancel_action = NULL;

  g_mutex_lock(&data_mutex);
  data_shared.new       = TRUE;
  data_shared.terminate = FALSE;
  data_shared.timeout   = TIMER_CANCEL;
  g_cond_signal(&data_cond);  /* hold lock before signalling */
  g_mutex_unlock(&data_mutex);

  /* Make cancel menu item insensitive. */
  cancel_action = gtk_action_group_get_action(pi->priv->action_group, timerMenuItems[TIMER_IDX_CANCEL].name);
  gtk_action_set_sensitive(cancel_action, FALSE);
}


static void
totem_timer_plugin_timerAdjustable(GtkAction *action, TotemTimerPlugin *pi) {
  GtkWidget     *dialog;
  GtkWidget     *label;
  GtkWidget     *spinButton;
  GtkAdjustment *adjustment;
  GtkWidget     *content_area;
  GtkWidget     *reject;
  gint           response;

  /* Build the dialog window */

  /* Add the buttons to the dialog window. */
  dialog = gtk_dialog_new_with_buttons("Configure Timer",
                                       totem_get_main_window(pi->priv->totem),
                                       GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                       "Abort"         , GTK_RESPONSE_REJECT,
                                       GTK_STOCK_APPLY , GTK_RESPONSE_APPLY,
                                       NULL);

  /* Add a stock cancel icon to the abort button. */
  reject = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_REJECT);
  gtk_button_set_image(GTK_BUTTON(reject),gtk_image_new_from_stock(GTK_STOCK_CANCEL, GTK_ICON_SIZE_BUTTON));

  /* Define a message (label) area. */
  label = gtk_label_new("\r\n"
"Enter the desired value (in minutes) for the timer.\r\n"
"'Apply' will start/restart the timer with the supplied value.\r\n"
"'Abort' will leave timer configuration unchanged.\r\n"
"\r\n");

  /* Define a spinButton. */
  adjustment = gtk_adjustment_new(TIMER_ADJ_DEFAULT, TIMER_MIN, TIMER_MAX, 1, 10, 0);
  spinButton = gtk_spin_button_new(adjustment, 10, 0);

  /* Add the message and spinButton to the content_area of the dialog window. */
  content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_add(GTK_CONTAINER(content_area), label);
  gtk_container_add(GTK_CONTAINER(content_area), spinButton);

  gtk_widget_show_all(dialog);
  response = gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_hide(dialog);
  if (GTK_RESPONSE_APPLY == response) {
    gint       time_raw;
    GtkAction *cancel_action = NULL;

    time_raw = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spinButton));
    if ((time_raw < TIMER_MIN) || (time_raw > TIMER_MAX)) {
      /* timer value extracted is out of range - (spin_button not defined properly) */
      /* handle this by using default timeout value */
      time_raw = TIMER_ADJ_DEFAULT;
    }

    g_mutex_lock(&data_mutex);
    data_shared.new       = TRUE;
    data_shared.terminate = FALSE;
    data_shared.timeout   = (TimeType) time_raw;
    g_cond_signal(&data_cond);  /* hold lock before signalling */
    g_mutex_unlock(&data_mutex);

    /* Make cancel menu item sensitive. */
    cancel_action = gtk_action_group_get_action(pi->priv->action_group, timerMenuItems[TIMER_IDX_CANCEL].name);
    gtk_action_set_sensitive(cancel_action, TRUE);
  }
  gtk_widget_destroy(dialog);
}


static void
totem_timer_plugin_timerFixed(GtkAction *action, TotemTimerPlugin *pi) {
  int        time_raw      = 0;    /* as extracted by sscanf */
  GtkAction *cancel_action = NULL;

  if (1 != sscanf(gtk_action_get_name(action), "%3dm", &time_raw)) {
    return; /* couldn't extract timer value from menu item name - (timerMenuItems[] is defined improperly) */
  }

  if ((time_raw < TIMER_MIN) || (time_raw > TIMER_MAX)) {
    return; /* timer value extracted is out of range - (timerMenuItems[] is defined improperly) */
  }

  g_mutex_lock(&data_mutex);
  data_shared.new       = TRUE;
  data_shared.terminate = FALSE;
  data_shared.timeout   = (TimeType) time_raw;
  g_cond_signal(&data_cond);  /* hold lock before signalling */
  g_mutex_unlock(&data_mutex);

  /* Make cancel menu item sensitive. */
  cancel_action = gtk_action_group_get_action(pi->priv->action_group, timerMenuItems[TIMER_IDX_CANCEL].name);
  gtk_action_set_sensitive(cancel_action, TRUE);
}


/* Called when the plugin is activated.
   Totem calls this when either the user activates the plugin,
   or when totem starts up with the plugin already configured as active. */
static void
impl_activate(PeasActivatable *plugin) {
  TotemTimerPlugin        *pi           = TOTEM_TIMER_PLUGIN(plugin);
  TotemTimerPluginPrivate *priv         = pi->priv;
  GtkActionEntry          *action_entry = NULL;
  GtkUIManager            *ui_manager   = NULL;
  GtkAction               *action       = NULL;
  guint                    i;
  guint                    j;

  priv->totem = g_object_get_data(G_OBJECT(plugin), "object");

  /* Build priv->action_entries[]. */
  priv->action_entries = g_malloc(NUM_ACTION_ENTRIES * sizeof(GtkActionEntry));

  action_entry = &(priv->action_entries[ACTION_IDX_MENU]);
  action_entry->accelerator = NULL;
  action_entry->name        = ACTION_NAME;
  action_entry->stock_id    = NULL;
  action_entry->tooltip     = NULL;
  action_entry->callback    = NULL;
  action_entry->label       = ACTION_NAME;

  action_entry = &(priv->action_entries[ACTION_IDX_CANCEL]);
  action_entry->accelerator = NULL;
  action_entry->name        = timerMenuItems[TIMER_IDX_CANCEL].name;
  action_entry->stock_id    = GTK_STOCK_CANCEL;
  action_entry->tooltip     = NULL;
  action_entry->callback    = G_CALLBACK(totem_timer_plugin_timerCancel);
  action_entry->label       = timerMenuItems[TIMER_IDX_CANCEL].name;

  action_entry = &(priv->action_entries[ACTION_IDX_ADJUST]);
  action_entry->accelerator = NULL;
  action_entry->name        = timerMenuItems[TIMER_IDX_ADJUST].name;
  action_entry->stock_id    = GTK_STOCK_PROPERTIES;
  action_entry->tooltip     = NULL;
  action_entry->callback    = G_CALLBACK(totem_timer_plugin_timerAdjustable);
  action_entry->label       = timerMenuItems[TIMER_IDX_ADJUST].name;

  for (i=ACTION_IDX_FIXED_START, j=TIMER_IDX_FIXED_START; i<NUM_ACTION_ENTRIES; i++, j++) {
    action_entry = &(priv->action_entries[i]);
    action_entry->accelerator = NULL;
    action_entry->name        = timerMenuItems[j].name;
    action_entry->stock_id    = NULL;
    action_entry->tooltip     = NULL;
    action_entry->callback    = G_CALLBACK(totem_timer_plugin_timerFixed);
    action_entry->label       = timerMenuItems[j].name;
  }

  /* Create the GUI */
  priv->action_group = gtk_action_group_new(ACTION_GROUP);
  gtk_action_group_add_actions(priv->action_group,
                               priv->action_entries,
                               NUM_ACTION_ENTRIES,
                               pi);

  ui_manager = totem_get_ui_manager(priv->totem);
  gtk_ui_manager_insert_action_group(ui_manager, priv->action_group, -1);
  g_object_unref(priv->action_group);

  priv->ui_merge_id = gtk_ui_manager_new_merge_id(ui_manager);

  /* Create Menu->Timer */
  gtk_ui_manager_add_ui(ui_manager,
                        priv->ui_merge_id,
                        "/ui/tmw-menubar/movie/save-placeholder",
                        ACTION_NAME,
                        ACTION_NAME,
                        GTK_UI_MANAGER_MENU,
                        TRUE);

  /* Add Timer to pop-up window */
  gtk_ui_manager_add_ui(ui_manager,
                        priv->ui_merge_id,
                        "/ui/totem-main-popup/save-placeholder",
                        ACTION_NAME,
                        ACTION_NAME,
                        GTK_UI_MANAGER_MENU,
                        FALSE);

  /* Add Timer sub-menu items to Menu->Timer and pop-up windows */
  for (i=0; i<(G_N_ELEMENTS(timerMenuItems)); i++) {
    gtk_ui_manager_add_ui(ui_manager,
                          priv->ui_merge_id,
                          "/ui/tmw-menubar/movie/save-placeholder/"ACTION_NAME,
                          timerMenuItems[i].name,
                          timerMenuItems[i].name,
                          GTK_UI_MANAGER_MENUITEM,
                          FALSE);

    gtk_ui_manager_add_ui(ui_manager,
                          priv->ui_merge_id,
                          "/ui/totem-main-popup/save-placeholder/"ACTION_NAME,
                          timerMenuItems[i].name,
                          timerMenuItems[i].name,
                          GTK_UI_MANAGER_MENUITEM,
                          FALSE);
  } /* for(i) */

  /* Make the entire timer menu sensitive. */
  action = gtk_action_group_get_action(priv->action_group, ACTION_NAME);
  gtk_action_set_sensitive(action, TRUE);

  /* Make cancel menu item insensitive. */
  action = gtk_action_group_get_action(priv->action_group, timerMenuItems[TIMER_IDX_CANCEL].name);
  gtk_action_set_sensitive(action, FALSE);

  /* Make sure shared data is in sane state before starting timer thread. */
  data_shared.new       = FALSE;
  data_shared.terminate = FALSE;
  data_shared.timeout   = TIMER_CANCEL;

  priv->timer_thread = g_thread_new("tTimerThread", (GThreadFunc) timer_function, (gpointer) priv->totem);
  if (!priv->timer_thread) {
    /* actually will not get here, g_thread_new() causes program abort if thread can not be created */
    return;
  }
}


/* Called when the plugin is activated.
   Totem calls this when either the user deactivates the plugin,
   or when totem exits with the plugin configured as active. */
static void
impl_deactivate(PeasActivatable *plugin) {
  TotemTimerPlugin        *pi         = TOTEM_TIMER_PLUGIN(plugin);
  TotemTimerPluginPrivate *priv       = pi->priv;
  GtkUIManager            *ui_manager = NULL;

  /* Tell the timer thread to exit gracefully. */
  g_mutex_lock(&data_mutex);
  data_shared.new       = TRUE;
  data_shared.terminate = TRUE;
  data_shared.timeout   = TIMER_CANCEL;  /* not used */
  g_cond_signal(&data_cond);  /* hold lock before signalling */
  g_mutex_unlock(&data_mutex);
  g_thread_join(priv->timer_thread);  /* g_thread_join() also does a g_thread_unref() too */

  ui_manager = totem_get_ui_manager(priv->totem);
  gtk_ui_manager_remove_ui(ui_manager, priv->ui_merge_id);
  gtk_ui_manager_remove_action_group(ui_manager, priv->action_group);

  priv->totem = NULL;

  g_free(priv->action_entries);
  priv->action_entries = NULL;
}
