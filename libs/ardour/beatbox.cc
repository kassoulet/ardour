#include <iostream>
#include <cstdio>
#include <cmath>
#include <cstring>

#include <unistd.h>
#include <stdint.h>

#include "pbd/i18n.h"

#include "evoral/midi_events.h"

#include "ardour/beatbox.h"
#include "ardour/midi_buffer.h"
#include "ardour/session.h"

using std::cerr;
using std::endl;

using namespace ARDOUR;

BeatBox::BeatBox (Session& s)
	: Processor (s, _("BeatBox"))
	, _start_requested (false)
	, _running (false)
	, _measures (2)
	, _tempo (120)
	, _tempo_request (0)
	, _meter_beats (4)
	, _meter_beat_type (4)
	, superclock_cnt (0)
	, last_start (0)
	, whole_note_superclocks (0)
	, beat_superclocks (0)
	, measure_superclocks (0)
	, _quantize_divisor (4)
	, clear_pending (false)
{
	_display_to_user = true;

	for (uint32_t n = 0; n < 1024; ++n) {
		event_pool.push_back (new Event());
	}
}

BeatBox::~BeatBox ()
{
}

void
BeatBox::compute_tempo_clocks ()
{
	whole_note_superclocks = (superclock_ticks_per_second * 60) / (_tempo / _meter_beat_type);
	beat_superclocks = whole_note_superclocks / _meter_beat_type;
	measure_superclocks = beat_superclocks * _meter_beats;
}

void
BeatBox::start ()
{
	/* compute tempo, beat steps etc. */

	compute_tempo_clocks ();

	/* we can start */

	_start_requested = true;
}

void
BeatBox::stop ()
{
	_start_requested = false;
}

void
BeatBox::set_tempo (float bpm)
{
	_tempo_request = bpm;
}

void
BeatBox::silence (framecnt_t, framepos_t)
{
	/* do nothing, we have no inputs or outputs */
}

void
BeatBox::run (BufferSet& bufs, framepos_t /*start_frame*/, framepos_t /*end_frame*/, double speed, pframes_t nsamples, bool /*result_required*/)
{
	if (bufs.count().n_midi() == 0) {
		return;
	}

	if (!_running) {
		if (_start_requested) {
			_running = true;
			last_start = superclock_cnt;
	}

	} else {
		if (!_start_requested) {
			_running = false;
		}
	}

	superclock_t superclocks = samples_to_superclock (nsamples, _session.frame_rate());

	if (_tempo_request) {
		double ratio = _tempo / _tempo_request;
		_tempo = _tempo_request;
		_tempo_request = 0;

		compute_tempo_clocks ();

		/* recompute all the event times based on the ratio between the
		 * new and old tempo.
		 */

		for (Events::iterator ee = _current_events.begin(); ee != _current_events.end(); ++ee) {
			(*ee)->time = llrintf ((*ee)->time * ratio);
		}
	}

	if (!_running) {
		superclock_cnt += superclocks;
		return;
	}

	superclock_t process_start = superclock_cnt - last_start;
	superclock_t process_end = process_start + superclocks;
	const superclock_t loop_length = _measures * measure_superclocks;
	const superclock_t orig_superclocks = superclocks;

	process_start %= loop_length;
	process_end   %= loop_length;

	bool two_pass_required;
	superclock_t offset = 0;

	if (process_end < process_start) {
		two_pass_required = true;
		process_end = loop_length;
		superclocks = process_end - process_start;
	} else {
		two_pass_required = false;
	}

	Evoral::Event<MidiBuffer::TimeType> in_event;

	/* do this on the first pass only */
	MidiBuffer& buf = bufs.get_midi (0);

  second_pass:

	/* Output */

	if (clear_pending) {

		for (Events::iterator ee = _current_events.begin(); ee != _current_events.end(); ++ee) {
			event_pool.push_back (*ee);
		}
		_current_events.clear ();
		_incomplete_notes.clear ();
		clear_pending = false;
	}

	for (Events::iterator ee = _current_events.begin(); ee != _current_events.end(); ++ee) {
		Event* e = (*ee);

		if (e->size && (e->time >= process_start && e->time < process_end)) {
			framepos_t sample_offset_in_buffer = superclock_to_samples (offset + e->time - process_start, _session.frame_rate());

			buf.push_back (sample_offset_in_buffer, e->size, e->buf);
		}

		if (e->time >= process_end) {
			break;
		}
	}

	/* input */

	for (MidiBuffer::iterator e = buf.begin(); e != buf.end(); ++e) {
		const Evoral::Event<MidiBuffer::TimeType>& in_event = *e;

		superclock_t event_time = superclock_cnt + samples_to_superclock (in_event.time(), _session.frame_rate());
		superclock_t elapsed_time = event_time - last_start;
		superclock_t in_loop_time = elapsed_time % loop_length;
		superclock_t quantized_time;

		if (_quantize_divisor != 0) {
			const superclock_t time_per_grid_unit = whole_note_superclocks / _quantize_divisor;

			if ((in_event.buffer()[0] & 0xf) == MIDI_CMD_NOTE_OFF) {

				/* note off is special - it must be quantized
				 * to at least 1 quantization "spacing" after
				 * the corresponding note on.
				 */

				/* look for the note on */

				IncompleteNotes::iterator ee;

				for (ee = _incomplete_notes.begin(); ee != _incomplete_notes.end(); ++ee) {
					/* check for same note and channel */
					if (((*ee)->buf[1] == in_event.buffer()[1]) && ((*ee)->buf[0] & 0xf) == (in_event.buffer()[0] & 0xf)) {
						quantized_time = (*ee)->time + time_per_grid_unit;
						_incomplete_notes.erase (ee);
						break;
					}
				}

				if (ee == _incomplete_notes.end()) {
					cerr << "Note off for " << (int) (*ee)->buf[1] << " seen without corresponding note on among " << _incomplete_notes.size() << endl;
					continue;
				}

			} else {
				quantized_time = (in_loop_time / time_per_grid_unit) * time_per_grid_unit;
			}

		} else {
			quantized_time = elapsed_time;
		}

		if (in_event.size() > 24) {
			cerr << "Ignored large MIDI event\n";
			continue;
		}

		if (event_pool.empty()) {
			cerr << "No more events, grow pool\n";
			continue;
		}

		Event* new_event = event_pool.back();
		event_pool.pop_back ();

		new_event->time = quantized_time;
		new_event->whole_note_superclocks = whole_note_superclocks;
		new_event->size = in_event.size();
		memcpy (new_event->buf, in_event.buffer(), new_event->size);

		inbound_tracker.track (new_event->buf);

		_current_events.insert (new_event);

		if ((new_event->buf[0] & 0xf) == MIDI_CMD_NOTE_ON) {
			_incomplete_notes.push_back (new_event);
		}
	}

	superclock_cnt += superclocks;

	if (two_pass_required) {
		offset = superclocks;
		superclocks = orig_superclocks - superclocks;
		process_start = 0;
		process_end = superclocks;
		two_pass_required = false;
		goto second_pass;
	}

	return;
}

void
BeatBox::set_quantize (int divisor)
{
	_quantize_divisor = divisor;
}

void
BeatBox::clear ()
{
	clear_pending = true;
}

bool
BeatBox::EventComparator::operator() (Event const * a, Event const *b) const
{
	if (a->time == b->time) {
		if (a->buf[0] == b->buf[0]) {
			return a < b;
		}
		return !ARDOUR::MidiBuffer::second_simultaneous_midi_byte_is_first (a->buf[0], b->buf[0]);
	}
	return a->time < b->time;
}

bool
BeatBox::can_support_io_configuration (const ChanCount& in, ChanCount& out)
{
	return true;
}

XMLNode&
BeatBox::get_state(void)
{
	return state (true);
}

XMLNode&
BeatBox::state(bool full)
{
	XMLNode& node = Processor::state(full);
	node.set_property ("type", "beatbox");

	return node;
}