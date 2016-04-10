/*
    Copyright (C) 2016 Paul Davis

    This program is free software; you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the Free
    Software Foundation; either version 2 of the License, or (at your option)
    any later version.

    This program is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "ardour/debug.h"
#include "ardour/mute_master.h"
#include "ardour/session.h"
#include "ardour/solo_control.h"

#include "i18n.h"

using namespace ARDOUR;
using namespace std;
using namespace PBD;

SoloControl::SoloControl (Session& session, std::string const & name, Soloable& s, Muteable& m)
	: SlavableAutomationControl (session, SoloAutomation, ParameterDescriptor (SoloAutomation),
	                             boost::shared_ptr<AutomationList>(new AutomationList(Evoral::Parameter(SoloAutomation))),
	                             name)
	, _soloable (s)
	, _muteable (m)
	, _self_solo (false)
	, _soloed_by_others_upstream (0)
	, _soloed_by_others_downstream (0)
{
	_list->set_interpolation (Evoral::ControlList::Discrete);
	/* solo changes must be synchronized by the process cycle */
	set_flags (Controllable::Flag (flags() | Controllable::RealTime));
}

void
SoloControl::set_self_solo (bool yn)
{
	DEBUG_TRACE (DEBUG::Solo, string_compose ("%1: set SELF solo => %2\n", name(), yn));
	_self_solo = yn;
	set_mute_master_solo ();
}

void
SoloControl::set_mute_master_solo ()
{
	_muteable.mute_master()->set_soloed_by_self (self_soloed());

	if (Config->get_solo_control_is_listen_control()) {
		_muteable.mute_master()->set_soloed_by_others (false);
	} else {
		_muteable.mute_master()->set_soloed_by_others (soloed_by_others_downstream() || soloed_by_others_upstream());
	}
}

void
SoloControl::master_changed (bool from_self, PBD::Controllable::GroupControlDisposition gcd)
{
	if (_soloable.is_safe() || !_soloable.can_solo()) {
		return;
	}

	bool master_soloed;

	{
		Glib::Threads::RWLock::ReaderLock lm (master_lock);
		master_soloed = (bool) get_masters_value_locked ();
	}

	/* Master is considered equivalent to an upstream solo control, not
	 * direct control over self-soloed.
	 */

	mod_solo_by_others_upstream (master_soloed ? 1 : -1);

	/* no need to call AutomationControl::master_changed() since it just
	   emits Changed() which we already did in mod_solo_by_others_upstream()
	*/
}

void
SoloControl::mod_solo_by_others_downstream (int32_t delta)
{
	if (_soloable.is_safe() || !_soloable.can_solo()) {
		return;
	}

	DEBUG_TRACE (DEBUG::Solo, string_compose ("%1 mod solo-by-downstream by %2, current up = %3 down = %4\n",
						  name(), delta, _soloed_by_others_upstream, _soloed_by_others_downstream));

	if (delta < 0) {
		if (_soloed_by_others_downstream >= (uint32_t) abs (delta)) {
			_soloed_by_others_downstream += delta;
		} else {
			_soloed_by_others_downstream = 0;
		}
	} else {
		_soloed_by_others_downstream += delta;
	}

	DEBUG_TRACE (DEBUG::Solo, string_compose ("%1 SbD delta %2 = %3\n", name(), delta, _soloed_by_others_downstream));

	set_mute_master_solo ();
	Changed (false, Controllable::UseGroup); /* EMIT SIGNAL */
}

void
SoloControl::mod_solo_by_others_upstream (int32_t delta)
{
	if (_soloable.is_safe() || !_soloable.can_solo()) {
		return;
	}

	DEBUG_TRACE (DEBUG::Solo, string_compose ("%1 mod solo-by-upstream by %2, current up = %3 down = %4\n",
	                                          name(), delta, _soloed_by_others_upstream, _soloed_by_others_downstream));

	uint32_t old_sbu = _soloed_by_others_upstream;

	if (delta < 0) {
		if (_soloed_by_others_upstream >= (uint32_t) abs (delta)) {
			_soloed_by_others_upstream += delta;
		} else {
			_soloed_by_others_upstream = 0;
		}
	} else {
		_soloed_by_others_upstream += delta;
	}

	DEBUG_TRACE (DEBUG::Solo, string_compose (
		             "%1 SbU delta %2 = %3 old = %4 sbd %5 ss %6 exclusive %7\n",
		             name(), delta, _soloed_by_others_upstream, old_sbu,
		             _soloed_by_others_downstream, _self_solo, Config->get_exclusive_solo()));


	/* push the inverse solo change to everything that feeds us.

	   This is important for solo-within-group. When we solo 1 track out of N that
	   feed a bus, that track will cause mod_solo_by_upstream (+1) to be called
	   on the bus. The bus then needs to call mod_solo_by_downstream (-1) on all
	   tracks that feed it. This will silence them if they were audible because
	   of a bus solo, but the newly soloed track will still be audible (because
	   it is self-soloed).

	   but .. do this only when we are being told to solo-by-upstream (i.e delta = +1),
		   not in reverse.
	*/

	if ((_self_solo || _soloed_by_others_downstream) &&
	    ((old_sbu == 0 && _soloed_by_others_upstream > 0) ||
	     (old_sbu > 0 && _soloed_by_others_upstream == 0))) {

		if (delta > 0 || !Config->get_exclusive_solo()) {
			_soloable.push_solo_upstream (delta);
		}
	}

	set_mute_master_solo ();
	Changed (false, Controllable::NoGroup); /* EMIT SIGNAL */
}

void
SoloControl::actually_set_value (double val, PBD::Controllable::GroupControlDisposition group_override)
{
	if (_soloable.is_safe() || !_soloable.can_solo()) {
		return;
	}

	set_self_solo (val == 1.0);

	/* this sets the Evoral::Control::_user_value for us, which will
	   be retrieved by AutomationControl::get_value (), and emits Changed
	*/

	AutomationControl::actually_set_value (val, group_override);
	_session.set_dirty ();
}

double
SoloControl::get_value () const
{
	if (slaved()) {
		Glib::Threads::RWLock::ReaderLock lm (master_lock);
		return get_masters_value_locked () ? 1.0 : 0.0;
	}

	if (_list && boost::dynamic_pointer_cast<AutomationList>(_list)->automation_playback()) {
		// Playing back automation, get the value from the list
		return AutomationControl::get_value();
	}

	return self_soloed() ? 1.0 : 0.0;
}

void
SoloControl::clear_all_solo_state ()
{
	// ideally this function will never do anything, it only exists to forestall Murphy

#ifndef NDEBUG
	// these are really debug messages, but of possible interest.
	if (self_soloed()) {
		PBD::info << string_compose (_("Cleared Explicit solo: %1\n"), name());
	}
	if (_soloed_by_others_upstream || _soloed_by_others_downstream) {
		PBD::info << string_compose (_("Cleared Implicit solo: %1 up:%2 down:%3\n"),
				name(), _soloed_by_others_upstream, _soloed_by_others_downstream);
	}
#endif

	_soloed_by_others_upstream = 0;
	_soloed_by_others_downstream = 0;

	set_self_solo (false);

	Changed (false, Controllable::UseGroup); /* EMIT SIGNAL */
}

int
SoloControl::set_state (XMLNode const & node, int)
{
	XMLProperty const * prop;

	if ((prop = node.property ("self-solo")) != 0) {
		set_self_solo (string_is_affirmative (prop->value()));
	}

	if ((prop = node.property ("soloed-by-upstream")) != 0) {
		_soloed_by_others_upstream = 0; // needed for mod_.... () to work
		mod_solo_by_others_upstream (atoi (prop->value()));
	}

	if ((prop = node.property ("soloed-by-downstream")) != 0) {
		_soloed_by_others_downstream = 0; // needed for mod_.... () to work
		mod_solo_by_others_downstream (atoi (prop->value()));
	}

	return 0;
}

XMLNode&
SoloControl::get_state ()
{
	XMLNode& node (SlavableAutomationControl::get_state());

	node.add_property (X_("self-solo"), _self_solo ? X_("yes") : X_("no"));
	char buf[32];
	snprintf (buf, sizeof(buf), "%d",  _soloed_by_others_upstream);
	node.add_property (X_("soloed-by-upstream"), buf);
	snprintf (buf, sizeof(buf), "%d",  _soloed_by_others_downstream);
	node.add_property (X_("soloed-by-downstream"), buf);

	return node;
}