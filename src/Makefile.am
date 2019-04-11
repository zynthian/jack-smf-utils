bin_PROGRAMS = jack-smf-player jack-smf-recorder
jack_smf_player_SOURCES = jack-smf-player.c
jack_smf_recorder_SOURCES = jack-smf-recorder.c
jack_smf_player_LDADD = $(GLIB_LIBS) $(GTHREAD_LIBS) $(JACK_LIBS) $(LASH_LIBS) ../libsmf/libsmf.a
jack_smf_recorder_LDADD = $(jack_smf_player_LDADD)
jack_smf_player_CFLAGS = $(GLIB_CFLAGS) $(GTHREAD_CFLAGS) $(JACK_CFLAGS) $(LASH_CFLAGS) \
	-I$(top_srcdir)/libsmf -DG_LOG_DOMAIN=\"jack-smf-player\"
jack_smf_recorder_CFLAGS = $(GLIB_CFLAGS) $(GTHREAD_CFLAGS) $(JACK_CFLAGS) $(LASH_CFLAGS) \
	-I$(top_srcdir)/libsmf -DG_LOG_DOMAIN=\"jack-smf-recorder\"

