/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <crm_internal.h>

#include <sys/param.h>
#include <crm/crm.h>
#include <crmd_fsa.h>

#include <sys/types.h>
#include <sys/wait.h>

#include <unistd.h>			/* for access */
#include <clplumbing/cl_signal.h>
#include <clplumbing/realtime.h>
#include <clplumbing/timers.h>
#include <sys/types.h>	/* for calls to open */
#include <sys/stat.h>	/* for calls to open */
#include <fcntl.h>	/* for calls to open */
#include <pwd.h>	/* for getpwuid */
#include <grp.h>	/* for initgroups */

#include <sys/time.h>	/* for getrlimit */
#include <sys/resource.h>/* for getrlimit */

#include <errno.h>

#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <crm/common/cluster.h>
#include <crmd_messages.h>
#include <crmd_callbacks.h>

#include <crm/cib.h>
#include <crmd.h>


#define PE_WORKING_DIR	HA_VARLIBDIR"/heartbeat/pengine"

struct crm_subsystem_s *pe_subsystem  = NULL;
void do_pe_invoke_callback(xmlNode *msg, int call_id, int rc,
			   xmlNode *output, void *user_data);


static void
save_cib_contents(xmlNode *msg, int call_id, int rc, xmlNode *output, void *user_data) 
{
    char *pid = user_data;
    register_fsa_error_adv(C_FSA_INTERNAL, I_ERROR, NULL, NULL, __FUNCTION__);
    
    if(rc == cib_ok) {
	char *filename = NULL;
	filename = generate_series_filename(PE_WORKING_DIR, "pe-core", crm_atoi(pid, 0), TRUE);

	if(write_xml_file(output, filename, TRUE) < 0) {
	    crm_err("Could not save CIB contents after PE crash to %s", filename);
	} else {
	    crm_notice("Saved CIB contents after PE crash to %s", filename);
	}

	crm_free(filename);
    }
    
    crm_free(pid);
}

static void
pe_connection_destroy(gpointer user_data)
{
    clear_bit_inplace(fsa_input_register, pe_subsystem->flag_connected);
    if(is_set(fsa_input_register, pe_subsystem->flag_required)) {
	crm_crit("Connection to the Policy Engine failed");

	if(pe_subsystem->pid > 0) {
	    int rc = cib_ok;
	    char *pid = crm_itoa(pe_subsystem->pid);
	    
	    /*
	     *The PE died...
	     *
	     * Save the current CIB so that we have a chance of
	     * figuring out what killed it.
	     *
	     * Delay raising the I_ERROR until the query below completes or
	     * 5s is up, whichever comes first.
	     *
	     */
	    rc = fsa_cib_conn->cmds->query(fsa_cib_conn, NULL, NULL, cib_scope_local);
	    fsa_cib_conn->cmds->register_callback(
		fsa_cib_conn, rc, 5, TRUE, pid, "save_cib_contents", save_cib_contents);

	} else {
	    register_fsa_error_adv(C_FSA_INTERNAL, I_ERROR, NULL, NULL, __FUNCTION__);
	}
	
    } else {
	crm_info("Connection to the Policy Engine released");
    }
    
    pe_subsystem->pid = -1;
    pe_subsystem->ipc = NULL;
    pe_subsystem->client = NULL;

    G_main_set_trigger(fsa_source);
    return;
}

static gboolean
pe_msg_dispatch(IPC_Channel *client, gpointer user_data) 
{
    xmlNode *msg = NULL;
    gboolean stay_connected = TRUE;
	
    while(IPC_ISRCONN(client)
	  && client->ops->is_message_pending(client)) {

	msg = xmlfromIPC(client, 0);
	if (msg != NULL) {
	    route_message(C_IPC_MESSAGE, msg);
	    free_xml(msg);
	}
    }
    
    if (client->ch_status != IPC_CONNECT) {
	crm_info("Received HUP from %s:[%d]", pe_subsystem->name, pe_subsystem->pid);	
	stay_connected = FALSE;
    }

    G_main_set_trigger(fsa_source);
    return stay_connected;
}

/*	 A_PE_START, A_PE_STOP, A_TE_RESTART	*/
void
do_pe_control(long long action,
	      enum crmd_fsa_cause cause,
	      enum crmd_fsa_state cur_state,
	      enum crmd_fsa_input current_input,
	      fsa_data_t *msg_data)
{
    static GCHSource *pe_source = NULL;
    struct crm_subsystem_s *this_subsys = pe_subsystem;

    long long stop_actions = A_PE_STOP;
    long long start_actions = A_PE_START;
    
    if(action & stop_actions) {
	clear_bit_inplace(fsa_input_register, pe_subsystem->flag_required);

	if(is_heartbeat_cluster()) {
	    stop_subsystem(this_subsys, FALSE);

	} else {    
	    if(pe_source) {
		G_main_del_IPC_Channel(pe_source);
		pe_source = NULL;
	    }
	    
	    if(pe_subsystem->ipc) {
		pe_subsystem->ipc->ops->destroy(pe_subsystem->ipc);
		pe_subsystem->ipc = NULL;
	    }
	    clear_bit_inplace(fsa_input_register, pe_subsystem->flag_connected);
	}
    }
    
    if(action & start_actions) {
	if(cur_state != S_STOPPING) {
	    if(is_openais_cluster()) {
		set_bit_inplace(fsa_input_register, pe_subsystem->flag_required);		

	    } else if(is_heartbeat_cluster()) {
		if(start_subsystem(this_subsys) == FALSE) {
		    register_fsa_error(C_FSA_INTERNAL, I_FAIL, NULL);
		    return;
		} 
		sleep(3);		
	    }

	    pe_subsystem->ipc = init_client_ipc_comms_nodispatch(CRM_SYSTEM_PENGINE);
	    if(pe_subsystem->ipc == NULL) {
		crm_warn("Setup of client connection failed,"
			 " not adding channel to mainloop");
		register_fsa_error(C_FSA_INTERNAL, I_FAIL, NULL);	    
		return;
	    }

	    set_bit_inplace(fsa_input_register, pe_subsystem->flag_connected);
	    pe_source = G_main_add_IPC_Channel(
		G_PRIORITY_HIGH, pe_subsystem->ipc, FALSE, pe_msg_dispatch, NULL, 
		pe_connection_destroy);
	    
	} else {
	    crm_info("Ignoring request to start %s while shutting down",
		     this_subsys->name);
	}
    }
}

int fsa_pe_query = 0;
char *fsa_pe_ref = NULL;

/*	 A_PE_INVOKE	*/
void
do_pe_invoke(long long action,
	     enum crmd_fsa_cause cause,
	     enum crmd_fsa_state cur_state,
	     enum crmd_fsa_input current_input,
	     fsa_data_t *msg_data)
{
	if(is_set(fsa_input_register, R_PE_CONNECTED) == FALSE){
		crm_info("Waiting for the PE to connect");
		crmd_fsa_stall(NULL);
		return;		
	}

	if(is_set(fsa_input_register, R_HAVE_CIB) == FALSE) {
		crm_err("Attempted to invoke the PE without a consistent"
			" copy of the CIB!");

		/* start the join from scratch */
		register_fsa_input_before(C_FSA_INTERNAL, I_ELECTION, NULL);
		return;		
	}
	
	crm_debug("Requesting the current CIB: %s",fsa_state2string(fsa_state));
	fsa_pe_query = fsa_cib_conn->cmds->query(
		fsa_cib_conn, NULL, NULL, cib_scope_local);
	if(FALSE == add_cib_op_callback(
		   fsa_cib_conn, fsa_pe_query, TRUE, NULL, do_pe_invoke_callback)) {
		crm_err("Cant retrieve the CIB to invoke the %s subsystem with",
			pe_subsystem->name);
		register_fsa_error(C_FSA_INTERNAL, I_ERROR, NULL);
	}
}

void
do_pe_invoke_callback(xmlNode *msg, int call_id, int rc,
		      xmlNode *output, void *user_data)
{
	xmlNode *cmd = NULL;

	if(call_id != fsa_pe_query) {
		crm_debug_2("Skipping superceeded CIB query: %d (current=%d)",
			    call_id, fsa_pe_query);
		return;
		
	} else if(AM_I_DC == FALSE
	   || is_set(fsa_input_register, R_PE_CONNECTED) == FALSE) {
		crm_debug("No need to invoke the PE anymore");
		return;

	} else if(fsa_state != S_POLICY_ENGINE) {
		crm_debug("Discarding PE request in state: %s",
			  fsa_state2string(fsa_state));
		return;

	} else if(last_peer_update != 0) {
	    crm_debug("Re-asking for the CIB: peer update %d still pending",
		      last_peer_update);
	    
	    mssleep(500);
	    register_fsa_action(A_PE_INVOKE);
	    return;

	} else if(fsa_state != S_POLICY_ENGINE) {
	    crm_err("Invoking PE in state: %s", fsa_state2string(fsa_state));
	}

	CRM_DEV_ASSERT(output != NULL);
	CRM_DEV_ASSERT(crm_element_value(output, XML_ATTR_DC_UUID) != NULL);

	crm_xml_add_int(output, XML_ATTR_HAVE_QUORUM, fsa_has_quorum);

	if(ever_had_quorum && crm_have_quorum == FALSE) {
	    crm_xml_add_int(output, XML_ATTR_QUORUM_PANIC, 1);	    
	}
	
	if(fsa_pe_ref) {
		crm_free(fsa_pe_ref);
		fsa_pe_ref = NULL;
	}

	cmd = create_request(CRM_OP_PECALC, output, NULL,
			     CRM_SYSTEM_PENGINE, CRM_SYSTEM_DC, NULL);

	fsa_pe_ref = crm_element_value_copy(cmd, XML_ATTR_REFERENCE);
	if(send_ipc_message(pe_subsystem->ipc, cmd) == FALSE) {
	    crm_err("Could not contact the pengine");
	    register_fsa_error_adv(C_FSA_INTERNAL, I_ERROR, NULL, NULL, __FUNCTION__);
	}
	
	crm_debug("Invoking the PE: ref=%s, seq=%llu, quorate=%d",
		  fsa_pe_ref, crm_peer_seq, fsa_has_quorum);
	free_xml(cmd);
}
