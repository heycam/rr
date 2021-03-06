/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#ifndef RR_GDB_SERVER_H_
#define RR_GDB_SERVER_H_

#include <map>
#include <memory>
#include <string>

#include "DiversionSession.h"
#include "GdbConnection.h"
#include "ReplaySession.h"
#include "ReplayTimeline.h"
#include "ScopedFd.h"
#include "TraceFrame.h"

class GdbServer {
public:
  struct Target {
    Target() : pid(0), require_exec(false), event(0) {}
    // Target process to debug, or 0 to just debug the first process
    pid_t pid;
    // If true, wait for the target process to exec() before attaching debugger
    bool require_exec;
    // Wait until at least 'event' has elapsed before attaching
    TraceFrame::Time event;
  };

  struct ConnectionFlags {
    // -1 to let GdbServer choose the port, a positive integer to select a
    // specific port to listen on.
    int dbg_port;
    // If non-null, then when the gdbserver is set up, we write its connection
    // parameters through this pipe. GdbServer::launch_gdb is passed the
    // other end of this pipe to exec gdb with the parameters.
    ScopedFd* debugger_params_write_pipe;

    ConnectionFlags() : dbg_port(-1), debugger_params_write_pipe(nullptr) {}
  };

  /**
   * Create a gdbserver serving the replay of 'session'.
   */
  GdbServer(std::shared_ptr<ReplaySession> session,
            const ReplaySession::Flags& flags, const Target& target)
      : target(target),
        stop_replaying_to_target(false),
        timeline(std::move(session), flags),
        emergency_debug_session(nullptr) {}

  /**
   * Actually run the server. Returns only when the debugger disconnects.
   */
  void serve_replay(const ConnectionFlags& flags);

  /**
   * exec()'s gdb using parameters read from params_pipe_fd (and sent through
   * the pipe passed to serve_replay_with_debugger).
   */
  static void launch_gdb(ScopedFd& params_pipe_fd,
                         const std::string& gdb_command_file_path);

  /**
   * Start a debugging connection for |t| and return when there are no
   * more requests to process (usually because the debugger detaches).
   *
   * This helper doesn't attempt to determine whether blocking rr on a
   * debugger connection might be a bad idea.  It will always open the debug
   * socket and block awaiting a connection.
   */
  static void emergency_debug(Task* t);

  /**
   * A string containing the default gdbinit script that we load into gdb.
   */
  static std::string init_script();

  /**
   * Called from a signal handler (or other thread) during serve_replay,
   * this will cause the replay-to-target phase to be interrupted and
   * debugging started wherever the replay happens to be.
   */
  void interrupt_replay_to_target() { stop_replaying_to_target = true; }

  /**
   * Return the register |which|, which may not have a defined value.
   */
  static GdbRegisterValue get_reg(const Registers& regs,
                                  const ExtraRegisters& extra_regs,
                                  GdbRegister which);

private:
  GdbServer(std::unique_ptr<GdbConnection>& dbg, Task* t)
      : dbg(std::move(dbg)),
        debuggee_tguid(t->task_group()->tguid()),
        last_continue_tuid(t->tuid()),
        last_query_tuid(t->tuid()),
        stop_replaying_to_target(false),
        emergency_debug_session(&t->session()) {}

  Session& current_session() {
    return timeline.is_running() ? timeline.current_session()
        : *emergency_debug_session;
  }

  /**
   * If |req| is a magic-write command, interpret it and return true.
   * Otherwise, do nothing and return false.
   */
  bool maybe_process_magic_command(const GdbRequest& req);
  /**
   * If |req| is a magic-read command, interpret it and return true.
   * Otherwise, do nothing and return false.
   */
  bool maybe_process_magic_read(Task* t, const GdbRequest& req);
  void dispatch_regs_request(const Registers& regs,
                             const ExtraRegisters& extra_regs);
  enum ReportState { REPORT_NORMAL, REPORT_THREADS_DEAD };
  /**
   * Process the single debugger request |req|, made by |dbg| targeting
   * |t|, inside the session |session|.
   *
   * Callers should implement any special semantics they want for
   * particular debugger requests before calling this helper, to do
   * generic processing.
   */
  void dispatch_debugger_request(Session& session, const GdbRequest& req,
                                 ReportState state);
  bool at_target();
  void activate_debugger();
  void restart_session(const GdbRequest& req);
  GdbRequest process_debugger_requests(ReportState state = REPORT_NORMAL);
  enum ContinueOrStop { CONTINUE_DEBUGGING, STOP_DEBUGGING };
  bool detach_or_restart(const GdbRequest& req, ContinueOrStop* s);
  ContinueOrStop handle_exited_state();
  ContinueOrStop debug_one_step(RunDirection* last_direction);
  /**
   * If 'req' is a reverse-singlestep, try to obtain the resulting state
   * directly from ReplayTimeline's mark database. If that succeeds,
   * report the singlestep break status to gdb and process any get-registers
   * requests. Repeat until we get a request that isn't reverse-singlestep
   * or get-registers, returning that request in 'req'.
   * During reverse-next commands, gdb tends to issue a series of
   * reverse-singlestep/get-registers pairs, and this makes those much
   * more efficient by avoiding having to actually reverse-singlestep the
   * session.
   */
  void try_lazy_reverse_singlesteps(GdbRequest& req);

  /**
   * Process debugger requests made in |diversion_session| until action needs
   * to be taken by the caller (a resume-execution request is received).
   * The received request is returned through |req|.
   * Returns true if diversion should continue, false if it should end.
   */
  bool diverter_process_debugger_requests(DiversionSession& diversion_session,
                                          uint32_t& diversion_refcount,
                                          GdbRequest* req);
  /**
   * Create a new diversion session using |replay| session as the
   * template.  The |replay| session isn't mutated.
   *
   * Execution begins in the new diversion session under the control of
   * |dbg| starting with initial thread target |task|.  The diversion
   * session ends at the request of |dbg|, and |divert| returns the first
   * request made that wasn't handled by the diversion session.  That
   * is, the first request that should be handled by |replay| upon
   * resuming execution in that session.
   */
  GdbRequest divert(ReplaySession& replay);

  /**
   * If break_status indicates a stop that we should report to gdb,
   * report it.
   */
  void maybe_notify_stop(const BreakStatus& break_status);

  /**
   * Return the checkpoint stored as |checkpoint_id| or nullptr if there
   * isn't one.
   */
  ReplaySession::shr_ptr get_checkpoint(int checkpoint_id);
  /**
   * Delete the checkpoint stored as |checkpoint_id| if it exists, or do
   * nothing if it doesn't exist.
   */
  void delete_checkpoint(int checkpoint_id);

  Target target;
  // dbg is initially null. Once the debugger connection is established, it
  // never changes.
  std::unique_ptr<GdbConnection> dbg;
  // When dbg is non-null, the TaskGroupUid of the task being debugged. Never
  // changes once the connection is established --- we don't currently
  // support switching gdb between debuggee processes.
  TaskGroupUid debuggee_tguid;
  // The TaskUid of the last continued task.
  TaskUid last_continue_tuid;
  // The TaskUid of the last queried task.
  TaskUid last_query_tuid;
  // True when the user has interrupted replaying to a target event.
  volatile bool stop_replaying_to_target;

  ReplayTimeline timeline;
  Session* emergency_debug_session;

  struct Checkpoint {
    Checkpoint(const ReplayTimeline::Mark& mark, TaskUid last_continue_tuid)
        : mark(mark), last_continue_tuid(last_continue_tuid) {}
    Checkpoint() = default;
    ReplayTimeline::Mark mark;
    TaskUid last_continue_tuid;
  };
  // |debugger_restart_mark| is the point where we will restart from with
  // a no-op debugger "run" command.
  Checkpoint debugger_restart_checkpoint;

  // gdb checkpoints, indexed by ID
  std::map<int, Checkpoint> checkpoints;
};

#endif /* RR_GDB_SERVER_H_ */
