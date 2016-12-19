#include <stdio.h>
#include <stdlib.h>
#include <jack/jack.h>
#include <math.h>
#include <string.h>

int sample_rate;

int n_inputs, n_outputs;
jack_port_t** in_ports;
jack_port_t** out_ports;

jack_default_audio_sample_t** in_bufs;
jack_default_audio_sample_t** out_bufs;

float T;

static inline void clear_buf(jack_default_audio_sample_t* buf, jack_nframes_t nframes)
{
	memset(buf, 0, sizeof(*buf) * nframes);
}

static int process(jack_nframes_t nframes, void* arg)
{
	for (int i = 0; i < n_inputs; i++) {
		clear_buf(in_bufs[i] = jack_port_get_buffer(in_ports[i], nframes), nframes);
	}
	for (int i = 0; i < n_outputs; i++) {
		clear_buf(out_bufs[i] = jack_port_get_buffer(out_ports[i], nframes), nframes);
	}

	for (int i = 0; i < n_inputs; i++) in_bufs[i] = jack_port_get_buffer(in_ports[i], nframes);
	for (int i = 0; i < n_outputs; i++) out_bufs[i] = jack_port_get_buffer(out_ports[i], nframes);

	for (int i = 0; i < nframes; i++) {
		float f = sinf(T) * 0.1f;

		for (int j = 0; j < n_outputs; j++) out_bufs[j][i] = f;
		T += 0.04;
		while (T > 6.2830f) T -= 6.2830f;
	}

	return 0;
}

static int xrun(void* arg)
{
	printf("xrun :-(\n");
	return 0;
}

static jack_port_t* register_port(jack_client_t* client, const char* fmt, int i, unsigned long flag)
{
	char name[16];
	snprintf(name, sizeof(name), fmt, i);
	return jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE, flag, 0);
}

static int connect_ports(jack_client_t* client, int is_input, int n, jack_port_t** local_ports)
{
	const char** remote_ports = jack_get_ports(client, NULL, NULL, JackPortIsPhysical | (is_input ? JackPortIsOutput : JackPortIsInput));
	if (remote_ports == NULL) return -1;

	for (int i = 0; i < n; i++) {
		const char* local_port = jack_port_name(local_ports[i]);

		const char* remote_port = remote_ports[i];
		if (remote_port == NULL) {
			free(remote_ports);
			return -1;
		}

		const char* src_port = is_input ? remote_port : local_port;
		const char* dst_port = is_input ? local_port : remote_port;

		if (jack_connect(client, src_port, dst_port) != 0) {
			free(remote_ports);
			return -1;
		}
	}

	free(remote_ports);

	return 0;
}

static int init(int _n_inputs, int _n_outputs)
{
	n_inputs = _n_inputs;
	n_outputs = _n_outputs;

	in_ports = calloc(n_inputs, sizeof(*in_ports));
	if (in_ports == NULL) return -1;

	in_bufs = calloc(n_inputs, sizeof(*in_bufs));
	if (in_bufs == NULL) return -1;

	out_ports = calloc(n_outputs, sizeof(*out_ports));
	if (out_ports == NULL) return -1;

	out_bufs = calloc(n_outputs, sizeof(*out_bufs));
	if (out_bufs == NULL) return -1;

	char* client_name = "do";
	jack_status_t status;
	jack_client_t* client = jack_client_open(client_name, JackNullOption, &status, NULL);
	if (client == NULL) {
		return -1;
	}
	if (status & JackNameNotUnique) {
		client_name = jack_get_client_name(client);
	}

	jack_set_process_callback(client, process, NULL);
	jack_set_xrun_callback(client, xrun, NULL);

	sample_rate = jack_get_sample_rate(client);

	for (int i = 0; i < n_inputs; i++) {
		in_ports[i] = register_port(client, "in_%d", i, JackPortIsInput);
		if (in_ports[i] == NULL) return -1;
	}

	for (int i = 0; i < n_outputs; i++) {
		out_ports[i] = register_port(client, "out_%d", i, JackPortIsOutput);
		if (out_ports[i] == NULL) return -1;
	}

	if (jack_activate(client) != 0) {
		return -1;
	}

	if (connect_ports(client, 1, n_inputs, in_ports) == -1) return -1;
	if (connect_ports(client, 0, n_outputs, out_ports) == -1) return -1;

	return 0;
}

int dd_jack_init()
{
	return init(2, 2);
}
