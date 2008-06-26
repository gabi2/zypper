// zypper - a command line interface for libzypp
// http://en.opensuse.org/User:Mvidner

// (initially based on dmacvicar's zmart)

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <iostream>
#include <fstream>
#include <sstream>
#include <streambuf>
#include <list>
#include <map>
#include <iterator>

#include <unistd.h>
#include <readline/history.h>

#include <boost/logic/tribool.hpp>
#include <boost/format.hpp>

#include "zypp/ZYppFactory.h"
#include "zypp/zypp_detail/ZYppReadOnlyHack.h"

#include "zypp/base/Logger.h"
#include "zypp/base/Algorithm.h"
#include "zypp/base/UserRequestException.h"

#include "zypp/sat/SolvAttr.h"
#include "zypp/PoolQuery.h"
#include "zypp/Locks.h"

#include "zypp/target/rpm/RpmHeader.h" // for install <.rpmURI>

#include "main.h"
#include "Zypper.h"
#include "repos.h"
#include "misc.h"
#include "locks.h"

#include "Table.h"
#include "search.h"
#include "info.h"
#include "getopt.h"
#include "Command.h"
#include "utils.h"

#include "output/OutNormal.h"
#include "output/OutXML.h"

using namespace std;
using namespace zypp;
using namespace boost;

ZYpp::Ptr God = NULL;
RuntimeData gData;
parsed_opts copts; // command options

static void rug_list_resolvables(Zypper & zypper);

Zypper::Zypper()
  : _argc(0), _argv(NULL), _out_ptr(NULL),
    _command(ZypperCommand::NONE),
    _exit_code(ZYPPER_EXIT_OK),
    _running_shell(false), _running_help(false), _exit_requested(false),
    _sh_argc(0), _sh_argv(NULL)
{
  MIL << "Hi, me zypper " VERSION " built " << __DATE__ << " " <<  __TIME__ << endl;
}


Zypper::~Zypper()
{
  delete _out_ptr;
  MIL << "Bye!" << endl;
}


Zypper::Ptr & Zypper::instance()
{
  static Zypper::Ptr _instance;
  
  if (!_instance)
    _instance.reset(new Zypper());
  else
    XXX << "Got an existing instance." << endl;

  return _instance;
}


int Zypper::main(int argc, char ** argv)
{
  _argc = argc;
  _argv = argv;

  // parse global options and the command
  try { processGlobalOptions(); }
  catch (const ExitRequestException & e)
  {
    MIL << "Caught exit request:" << endl << e.msg() << endl;
    return exitCode();
  }
  
  if (runningHelp())
  {
    safeDoCommand();
    return exitCode();
  }

  switch(command().toEnum())
  {
  case ZypperCommand::SHELL_e:
    commandShell();
    cleanup();
    return exitCode();

  case ZypperCommand::NONE_e:
    return ZYPPER_EXIT_ERR_SYNTAX;

  default:
    safeDoCommand();
    cleanup();
    return exitCode();
  }

  WAR << "This line should never be reached." << endl;

  return exitCode();
}

Out & Zypper::out()
{
  if (_out_ptr)
    return *_out_ptr;
  
  cerr << "uninitialized output writer" << endl;
  ZYPP_THROW(ExitRequestException("no output writer"));
}

void print_main_help(Zypper & zypper)
{
  static string help_global_options = _("  Global Options:\n"
    "\t--help, -h\t\tHelp.\n"
    "\t--version, -V\t\tOutput the version number.\n"
    "\t--quiet, -q\t\tSuppress normal output, print only error messages.\n"
    "\t--verbose, -v\t\tIncrease verbosity.\n"
    "\t--no-abbrev, -A\t\tDo not abbreviate text in tables.\n"
    "\t--table-style, -s\tTable style (integer).\n"
    "\t--rug-compatible, -r\tTurn on rug compatibility.\n"
    "\t--non-interactive, -n\tDo not ask anything, use default answers automatically.\n"
    "\t--xmlout, -x\t\tSwitch to XML output.\n"  
    "\t--reposd-dir, -D <dir>\tUse alternative repository definition files directory.\n"
    "\t--cache-dir, -C <dir>\tUse alternative meta-data cache database directory.\n"
    "\t--raw-cache-dir <dir>\tUse alternative raw meta-data cache directory.\n"
  );

  static string help_global_source_options = _(
    "\tRepository Options:\n"
    "\t--no-gpg-checks\t\tIgnore GPG check failures and continue.\n"
    "\t--plus-repo, -p <URI>\tUse an additional repository.\n"
    "\t--disable-repositories\tDo not read meta-data from repositories.\n"
    "\t--no-refresh\t\tDo not refresh the repositories.\n"
  );

  static string help_global_target_options = _("\tTarget Options:\n"
    "\t--root, -R <dir>\tOperate on a different root directory.\n"
    "\t--disable-system-resolvables  Do not read installed resolvables.\n"
  );

  static string help_commands = _(
    "  Commands:\n"
    "\thelp, ?\t\t\tPrint help.\n"
    "\tshell, sh\t\tAccept multiple commands at once.\n"
  );

  static string help_repo_commands = _("\tRepository Handling:\n"
    "\trepos, lr\t\tList all defined repositories.\n"
    "\taddrepo, ar\t\tAdd a new repository.\n"
    "\tremoverepo, rr\t\tRemove specified repository.\n"
    "\trenamerepo, nr\t\tRename specified repository.\n"
    "\tmodifyrepo, mr\t\tModify specified repository.\n"
    "\trefresh, ref\t\tRefresh all repositories.\n"
    "\tclean\t\t\tClean local caches.\n"
  );
  
  static string help_package_commands = _("\tSoftware Management:\n"
    "\tinstall, in\t\tInstall packages.\n"
    "\tremove, rm\t\tRemove packages.\n"
    "\tverify, ve\t\tVerify integrity of package dependencies.\n"
    "\tupdate, up\t\tUpdate installed packages with newer versions.\n"
    "\tdist-upgrade, dup\tPerform a distribution upgrade.\n"
    "\tsource-install, si\tInstall source packages and their build dependencies.\n"
    "\tinstall-new-recommends, inr  Install newly added packages recommended by installed packages.\n"
  );

  static string help_query_commands = _("\tQuerying:\n"
    "\tsearch, se\t\tSearch for packages matching a pattern.\n"
    "\tinfo, if\t\tShow full information for specified packages.\n"
    "\tpatch-info\t\tShow full information for specified patches.\n"
    "\tpattern-info\t\tShow full information for specified patterns.\n"
    "\tproduct-info\t\tShow full information for specified products.\n"
    "\tpatch-check, pchk\tCheck for patches.\n"
    "\tlist-updates, lu\tList available updates.\n"
    "\tpatches, pch\t\tList all available patches.\n"
    "\tpackages, pa\t\tList all available packages.\n"
    "\tpatterns, pt\t\tList all available patterns.\n"
    "\tproducts, pd\t\tList all available products.\n"
    "\twhat-provides, wp\tList packages providing specified capability.\n"
    //"\twhat-requires, wr\tList packages requiring specified capability.\n"
    //"\twhat-conflicts, wc\tList packages conflicting with specified capability.\n"
  );

  static string help_lock_commands = _("\tPackage Locks:\n"
    "\taddlock, al\t\tAdd a package lock.\n"
    "\tremovelock, rl\t\tRemove a package lock.\n"
    "\tlocks, ll\t\tList current package locks.\n"
  );
  static string help_usage = _(
    "  Usage:\n"
    "\tzypper [--global-options] <command> [--command-options] [arguments]\n"
  );

  zypper.out().info(help_usage, Out::QUIET);
  zypper.out().info(help_global_options, Out::QUIET);
  zypper.out().info(help_global_source_options, Out::QUIET);
  zypper.out().info(help_global_target_options, Out::QUIET);
  zypper.out().info(help_commands, Out::QUIET);
  zypper.out().info(help_repo_commands, Out::QUIET);
  zypper.out().info(help_package_commands, Out::QUIET);
  zypper.out().info(help_query_commands, Out::QUIET);
  zypper.out().info(help_lock_commands, Out::QUIET);

  print_command_help_hint(zypper);
}

void print_unknown_command_hint(Zypper & zypper)
{
  zypper.out().info(boost::str(format(
    // translators: %s is "help" or "zypper help" depending on whether
    // zypper shell is running or not
    _("Type '%s' to get a list of global options and commands."))
      % (zypper.runningShell() ? "help" : "zypper help")));
}

void print_command_help_hint(Zypper & zypper)
{
  zypper.out().info(boost::str(format(
    // translators: %s is "help" or "zypper help" depending on whether
    // zypper shell is running or not
    _("Type '%s' to get command-specific help."))
      % (zypper.runningShell() ? "help <command>" : "zypper help <command>")));
}

/*
 * parses global options, returns the command
 * 
 * \returns ZypperCommand object representing the command or ZypperCommand::NONE
 *          if an unknown command has been given. 
 */
void Zypper::processGlobalOptions()
{
  MIL << "START" << endl;

  static struct option global_options[] = {
    {"help",                       no_argument,       0, 'h'},
    {"verbose",                    no_argument,       0, 'v'},
    {"quiet",                      no_argument,       0, 'q'},
    {"version",                    no_argument,       0, 'V'},
    // rug compatibility alias for -vv
    {"debug",                      no_argument,       0,  0 },
    // rug compatibility alias for the default output level => ignored
    {"normal-output",              no_argument,       0,  0 },
    // not implemented currently => ignored
    {"terse",                      no_argument,       0, 't'},
    {"no-abbrev",                  no_argument,       0, 'A'},
    {"table-style",                required_argument, 0, 's'},
    {"rug-compatible",             no_argument,       0, 'r'},
    {"non-interactive",            no_argument,       0, 'n'},
    {"no-gpg-checks",              no_argument,       0,  0 },
    {"root",                       required_argument, 0, 'R'},
    {"reposd-dir",                 required_argument, 0, 'D'},
    {"cache-dir",                  required_argument, 0, 'C'},
    {"raw-cache-dir",              required_argument, 0,  0 },
    {"opt",                        optional_argument, 0, 'o'},
    // TARGET OPTIONS
    {"disable-system-resolvables", no_argument,       0,  0 },
    // REPO OPTIONS
    {"plus-repo",                  required_argument, 0, 'p'},
    {"disable-repositories",       no_argument,       0,  0 },
    {"no-refresh",                 no_argument,       0,  0 },
    {"xmlout",                     no_argument,       0, 'x'},
    {0, 0, 0, 0}
  };

  // parse global options
  parsed_opts gopts = parse_options (_argc, _argv, global_options);
  if (gopts.count("_unknown"))
  {
    setExitCode(ZYPPER_EXIT_ERR_SYNTAX);
    return;
  }

  parsed_opts::const_iterator it;

  // ====== output setup ======
  // depends on global options, that's we set it up here
  //! \todo create a default in the zypper constructor, recreate here.

  // determine the desired verbosity
  int iverbosity = 0;
  //// --quiet
  if (gopts.count("quiet"))
  {
    _gopts.verbosity = iverbosity = -1;
    DBG << "Verbosity " << _gopts.verbosity << endl;
  }
  //// --verbose
  if ((it = gopts.find("verbose")) != gopts.end())
  {
    //! \todo if iverbosity is -1 now, say we conflict with -q
    _gopts.verbosity += iverbosity = it->second.size();
    // _gopts.verbosity += gopts["verbose"].size();
  }

  Out::Verbosity verbosity = Out::NORMAL;
  switch(iverbosity)
  {
    case -1: verbosity = Out::QUIET; break;
    case 0: verbosity = Out::NORMAL; break;
    case 1: verbosity = Out::HIGH; break;
    default: verbosity = Out::DEBUG;
  }

  //// --debug
  // rug compatibility alias for -vv
  if (gopts.count("debug"))
    verbosity = Out::DEBUG;

  // create output object

  //// --xml-out
  if (gopts.count("xmlout"))
  {
    _out_ptr = new OutXML(verbosity);
    _gopts.machine_readable = true;
    _gopts.no_abbrev = true;
  }
  else
    _out_ptr = new OutNormal(verbosity);

  out().info(boost::str(format(_("Verbosity: %d")) % _gopts.verbosity), Out::HIGH);
  DBG << "Verbosity " << verbosity << endl;
  DBG << "Output type " << _out_ptr->type() << endl;

  if (gopts.count("no-abbrev"))
    _gopts.no_abbrev = true;

  if ((it = gopts.find("table-style")) != gopts.end())
  {
    unsigned s;
    str::strtonum (it->second.front(), s);
    if (s < _End)
      Table::defaultStyle = (TableStyle) s;
    else
      out().error(str::form(_("Invalid table style %d."), s),
          str::form(_("Use an integer number from %d to %d"), 0, 8));
  }

  if (gopts.count("terse")) 
  {
    _gopts.machine_readable = true;
    _gopts.no_abbrev = true;
    out().error("--terse is not implemented, does nothing");
  }

  // ======== other global options ========
  
  string rug_test(_argv[0]);
  if (gopts.count("rug-compatible") || rug_test.rfind("rug") == rug_test.size()-3 )
  {
    _gopts.is_rug_compatible = true;
    out().info("Switching to rug-compatible mode.", Out::DEBUG);
    DBG << "Switching to rug-compatible mode." << endl;
  }

  // Help is parsed by setting the help flag for a command, which may be empty
  // $0 -h,--help
  // $0 command -h,--help
  // The help command is eaten and transformed to the help option
  // $0 help
  // $0 help command
  if (gopts.count("help"))
    setRunningHelp(true);

  if (gopts.count("non-interactive")) {
    _gopts.non_interactive = true;
    out().info(_("Entering non-interactive mode."), Out::HIGH);
    MIL << "Entering non-interactive mode" << endl;
  }

  if (gopts.count("no-gpg-checks")) {
    _gopts.no_gpg_checks = true;
    out().info(_("Entering 'no-gpg-checks' mode."), Out::HIGH);
    MIL << "Entering no-gpg-checks mode" << endl;
  }

  if ((it = gopts.find("root")) != gopts.end()) {
    _gopts.root_dir = it->second.front();
    _gopts.changedRoot = true;
    Pathname tmp(_gopts.root_dir);
    if (!tmp.absolute())
    {
      out().error(
        _("The path specified in the --root option must be absolute."));
      _exit_code = ZYPPER_EXIT_ERR_INVALID_ARGS;
      return;
    }

    DBG << "root dir = " << _gopts.root_dir << endl;
    _gopts.rm_options = RepoManagerOptions(_gopts.root_dir);
  }

  if ((it = gopts.find("reposd-dir")) != gopts.end()) {
    _gopts.rm_options.knownReposPath = it->second.front();
  }

  if ((it = gopts.find("cache-dir")) != gopts.end()) {
    _gopts.rm_options.repoCachePath = it->second.front();
  }

  if ((it = gopts.find("raw-cache-dir")) != gopts.end()) {
    _gopts.rm_options.repoRawCachePath = it->second.front();
  }

  DBG << "repos.d dir = " << _gopts.rm_options.knownReposPath << endl;
  DBG << "cache dir = " << _gopts.rm_options.repoCachePath << endl;
  DBG << "raw cache dir = " << _gopts.rm_options.repoRawCachePath << endl;

  if (gopts.count("disable-repositories"))
  {
    MIL << "Repositories disabled, using target only." << endl;
    out().info(
      _("Repositories disabled, using the database of installed packages only."),
      Out::HIGH);
    _gopts.disable_system_sources = true;
  }
  else
  {
    MIL << "Repositories enabled" << endl;
  }

  if (gopts.count("no-refresh"))
  {
    _gopts.no_refresh = true;
    out().info(_("Autorefresh disabled."), Out::HIGH);
    MIL << "Autorefresh disabled." << endl;
  }

  if (gopts.count("disable-system-resolvables"))
  {
    MIL << "System resolvables disabled" << endl;
    out().info(_("Ignoring installed resolvables."), Out::HIGH);
    _gopts.disable_system_resolvables = true;
  }

  // testing option
  if ((it = gopts.find("opt")) != gopts.end()) {
    cout << "Opt arg: ";
    std::copy (it->second.begin(), it->second.end(),
               ostream_iterator<string> (cout, ", "));
    cout << endl;
  }

  // get command
  if (optind < _argc)
  {
    try { setCommand(ZypperCommand(_argv[optind++])); }
    // exception from command parsing
    catch (Exception & e)
    {
      out().error(e.asUserString());
    }
  }
  else if (!gopts.count("version"))
    setRunningHelp();

  if (command() == ZypperCommand::HELP)
  {
    setRunningHelp(true);
    if (optind < _argc)
    {
      string arg = _argv[optind++];
      try { setCommand(ZypperCommand(arg)); }
      catch (Exception e)
      {
        if (!arg.empty() && arg != "-h" && arg != "--help")
        {
          out().info(e.asUserString(), Out::QUIET);
          print_unknown_command_hint(*this);
          ZYPP_THROW(ExitRequestException("help provided"));
        }
      }
    }
    else
    {
      print_main_help(*this);
      ZYPP_THROW(ExitRequestException("help provided"));
    }
  }
  else if (command() == ZypperCommand::NONE)
  {
    if (runningHelp())
      print_main_help(*this);
    else if (gopts.count("version"))
    {
      out().info(PACKAGE " " VERSION, Out::QUIET);
      ZYPP_THROW(ExitRequestException("version shown"));
    }
    else
    {
      print_unknown_command_hint(*this);
      setExitCode(ZYPPER_EXIT_ERR_SYNTAX);
    }
  }
  else if (command() == ZypperCommand::SHELL && optind < _argc)
  {
    string arg = _argv[optind++];
    if (!arg.empty())
    {
      if (arg == "-h" || arg == "--help")
        setRunningHelp(true);
      else
      {
        report_too_many_arguments("shell\n");
        setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
        ZYPP_THROW(ExitRequestException("help provided"));
      }
    }
  }

  // additional repositories
  if (gopts.count("plus-repo"))
  {
    switch (command().toEnum())
    {
    case ZypperCommand::ADD_REPO_e:
    case ZypperCommand::REMOVE_REPO_e:
    case ZypperCommand::MODIFY_REPO_e:
    case ZypperCommand::RENAME_REPO_e:
    case ZypperCommand::REFRESH_e:
    case ZypperCommand::CLEAN_e:
    case ZypperCommand::REMOVE_LOCK_e:
    case ZypperCommand::LIST_LOCKS_e:
    {
      out().warning(boost::str(format(
        // TranslatorExplanation The %s is "--plus-repo"
        _("The %s option has no effect here, ignoring."))
        % "--plus-repo"));
      break;
    }
    default:
    {
      list<string> repos = gopts["plus-repo"];

      int count = 1;
      for (list<string>::const_iterator it = repos.begin();
          it != repos.end(); ++it)
      {
        Url url = make_url (*it);
        if (!url.isValid())
        {
          setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
          return;
        }

        RepoInfo repo;
        repo.addBaseUrl(url);
        repo.setEnabled(true);
        repo.setAutorefresh(true);
        repo.setAlias(boost::str(format("tmp%d") % count));
        repo.setName(url.asString());

        gData.additional_repos.push_back(repo);
        DBG << "got additional repo: " << url << endl;
        count++;
      }
    }
    }
  }

  MIL << "DONE" << endl;
}


void Zypper::commandShell()
{
  MIL << "Entering the shell" << endl;

  setRunningShell(true);

  string histfile;
  try {
    Pathname p (getenv ("HOME"));
    p /= ".zypper_history";
    histfile = p.asString ();
  } catch (...) {
    // no history
  }
  using_history ();
  if (!histfile.empty ())
    read_history (histfile.c_str ());

  while (true) {
    // read a line
    string line = readline_getline ();
    out().info(boost::str(format("Got: %s") % line), Out::DEBUG);
    // reset optind etc
    optind = 0;
    // split it up and create sh_argc, sh_argv
    Args args(line);
    _sh_argc = args.argc();
    _sh_argv = args.argv();

    string command_str = _sh_argv[0] ? _sh_argv[0] : "";

    if (command_str == "\004") // ^D
    {
      cout << endl; // print newline after ^D
      break;
    }

    try
    {
      setCommand(ZypperCommand(command_str));
      if (command() == ZypperCommand::SHELL_QUIT)
        break;
      else if (command() == ZypperCommand::NONE)
        print_unknown_command_hint(*this);
      else
        safeDoCommand();
    }
    catch (const Exception & e)
    {
      out().error(e.msg());
      print_unknown_command_hint(*this);
    }

    shellCleanup();
  }

  if (!histfile.empty ())
    write_history (histfile.c_str ());

  MIL << "Leaving the shell" << endl;
  setRunningShell(false);
}

void Zypper::shellCleanup()
{
  MIL << "Cleaning up for the next command." << endl;

  switch(command().toEnum())
  {
  case ZypperCommand::INSTALL_e:
  case ZypperCommand::REMOVE_e:
  case ZypperCommand::UPDATE_e:
  {
    remove_selections(*this);
    break;
  }
  default:;
  }

  // clear any previous arguments 
  _arguments.clear();
  // clear command options
  if (!_copts.empty())
  {
    _copts.clear();
    _cmdopts = CommandOptions();
  }
  // clear the command
  _command = ZypperCommand::NONE;
  // clear command help text
  _command_help.clear();
  // reset help flag
  setRunningHelp(false);
  // ... and the exit code
  setExitCode(ZYPPER_EXIT_OK);

  // gData
  gData.current_repo = RepoInfo();

  // TODO:
  // gData.repos re-read after repo operations or modify/remove these very repoinfos 
  // gData.repo_resolvables re-read only after certain repo operations (all?)
  // gData.target_resolvables re-read only after installation/removal/update
  // call target commit refresh pool after installation/removal/update (#328855)
}


/// process one command from the OS shell or the zypper shell
// catch unexpected exceptions and tell the user to report a bug (#224216)
void Zypper::safeDoCommand()
{
  try
  {
    processCommandOptions();
    if (command() == ZypperCommand::NONE || exitCode())
      return;
    doCommand();
  }
  catch (const AbortRequestException & ex)
  {
    ZYPP_CAUGHT(ex);

    // << _("User requested to abort.") << endl;
    out().error(ex.asUserString());
  }
  catch (const ExitRequestException & e)
  {
    MIL << "Caught exit request:" << endl << e.msg() << endl; 
  }
  catch (const Exception & ex)
  {
    ZYPP_CAUGHT(ex);

    Out::Verbosity tmp = out().verbosity(); 
    out().setVerbosity(Out::DEBUG);
    out().error(ex, _("Unexpected exception."));
    out().setVerbosity(tmp);

    report_a_bug(out());
  }
}

// === command-specific options ===
void Zypper::processCommandOptions()
{
  MIL << "START" << endl;

  struct option no_options = {0, 0, 0, 0};
  struct option *specific_options = &no_options;

  // this could be done in the processGlobalOptions() if there was no
  // zypper shell
  if (command() == ZypperCommand::HELP)
  {
    if (argc() > 1)
    {
      string cmd = argv()[1];
      try
      {
        setRunningHelp(true);
        setCommand(ZypperCommand(cmd));
      }
      catch (Exception & ex) {
        if (!cmd.empty() && cmd != "-h" && cmd != "--help")
        {
          out().info(ex.asUserString(), Out::QUIET);
          print_unknown_command_hint(*this);
          return;
        }
      }
    }
    else
    {
      print_main_help(*this);
      return;
    }
  }

  switch (command().toEnum())
  {

  // print help on help
  case ZypperCommand::HELP_e:
  {
    print_unknown_command_hint(*this);
    print_command_help_hint(*this);
    break;
  }

  case ZypperCommand::INSTALL_e:
  {
    static struct option install_options[] = {
      {"repo",                      required_argument, 0, 'r'},
      // rug compatibility option, we have --repo
      {"catalog",                   required_argument, 0, 'c'},
      {"type",                      required_argument, 0, 't'},
      // the default (ignored)
      {"name",                      no_argument,       0, 'n'},
      {"force",                     no_argument,       0, 'f'},
      {"capability",                no_argument,       0, 'C'},
      // rug compatibility, we have global --non-interactive
      {"no-confirm",                no_argument,       0, 'y'},
      {"auto-agree-with-licenses",  no_argument,       0, 'l'},
      // rug compatibility, we have --auto-agree-with-licenses
      {"agree-to-third-party-licenses",  no_argument,  0, 0},
      {"debug-solver",              no_argument,       0, 0},
      {"no-force-resolution",       no_argument,       0, 'R'},
      {"force-resolution",          no_argument,       0,  0 },
      {"dry-run",                   no_argument,       0, 'D'},
      // rug uses -N shorthand
      {"dry-run",                   no_argument,       0, 'N'},
      {"no-recommends",             no_argument,       0,  0 },
      {"help",                      no_argument,       0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = install_options;
    _command_help = str::form(_(
      // TranslatorExplanation the first %s = "package, patch, pattern, product"
      //  and the second %s = "package"
      "install (in) [options] <capability|rpm_file_uri> ...\n"
      "\n"
      "Install resolvables with specified capabilities or RPM files with"
      " specified location. A capability is"
      " NAME[OP<VERSION>], where OP is one of <, <=, =, >=, >.\n"
      "\n"
      "  Command options:\n"
      "-r, --repo <alias|#|URI>        Install resolvables only from the specified repository.\n"
      "-t, --type <type>               Type of resolvable (%s).\n"
      "                                Default: %s.\n"
      "-n, --name                      Select resolvables by plain name, not by capability.\n"
      "-C, --capability                Select resolvables by capability.\n"
      "-f, --force                     Install even if the item is already installed (reinstall).\n"
      "-l, --auto-agree-with-licenses  Automatically say 'yes' to third party license confirmation prompt.\n"
      "                                See 'man zypper' for more details.\n"
      "    --debug-solver              Create solver test case for debugging.\n"
      "    --no-recommends             Do not install recommended packages, only required.\n"
      "-R, --no-force-resolution       Do not force the solver to find solution, let it ask.\n"
      "    --force-resolution          Force the solver to find a solution (even an agressive).\n"
      "-D, --dry-run                   Test the installation, do not actually install.\n"
    ), "package, patch, pattern, product", "package");
    break;
  }

  case ZypperCommand::REMOVE_e:
  {
    static struct option remove_options[] = {
      {"repo",       required_argument, 0, 'r'},
      // rug compatibility option, we have --repo
      {"catalog",    required_argument, 0, 'c'},
      {"type",       required_argument, 0, 't'},
      // the default (ignored)
      {"name",       no_argument,       0, 'n'},
      {"capability", no_argument,       0, 'C'},
      // rug compatibility, we have global --non-interactive
      {"no-confirm", no_argument,       0, 'y'},
      {"debug-solver", no_argument,     0, 0},
      {"no-force-resolution", no_argument, 0, 'R'},
      {"force-resolution", no_argument, 0,  0 },
      {"dry-run",    no_argument,       0, 'D'},
      // rug uses -N shorthand
      {"dry-run",    no_argument,       0, 'N'},
      {"help",       no_argument,       0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = remove_options;
    _command_help = str::form(_(
      // TranslatorExplanation the first %s = "package, patch, pattern, product"
      //  and the second %s = "package"
      "remove (rm) [options] <capability> ...\n"
      "\n"
      "Remove resolvables with specified capabilities. A capability is"
      " NAME[OP<VERSION>], where OP is one of <, <=, =, >=, >.\n"
      "\n"
      "  Command options:\n"
      "-r, --repo <alias|#|URI>        Operate only with resolvables from the specified repository.\n"
      "-t, --type <type>               Type of resolvable (%s).\n"
      "                                Default: %s.\n"
      "-n, --name                      Select resolvables by plain name, not by capability.\n"
      "-C, --capability                Select resolvables by capability.\n"
      "    --debug-solver              Create solver test case for debugging.\n"  
      "-R, --no-force-resolution       Do not force the solver to find solution, let it ask.\n"
      "    --force-resolution          Force the solver to find a solution (even an agressive).\n"
      "-D, --dry-run                   Test the removal, do not actually remove.\n"
    ), "package, patch, pattern, product", "package");
    break;
  }

  case ZypperCommand::SRC_INSTALL_e:
  {
    static struct option src_install_options[] = {
      {"build-deps-only", no_argument, 0, 'd'},
      {"no-build-deps", no_argument, 0, 'D'},
      {"repo", required_argument, 0, 'r'},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = src_install_options;
    _command_help = _(
      "source-install (si) [options] <name> ...\n"
      "\n"
      "Install specified source packages and their build dependencies.\n"
      "\n"
      "  Command options:\n"
      "-d, --build-deps-only    Install only build dependencies of specified packages.\n"
      "-D, --no-build-deps      Don't install build dependencies.\n"
      "-r, --repo <alias|#|URI> Install packages only from specified repositories.\n"
    );
    break;
  }

  case ZypperCommand::VERIFY_e:
  {
    static struct option verify_options[] = {
      // rug compatibility option, we have global --non-interactive
      {"no-confirm", no_argument, 0, 'y'},
      {"dry-run", no_argument, 0, 'D'},
      // rug uses -N shorthand
      {"dry-run", no_argument, 0, 'N'},
      {"repo", required_argument, 0, 'r'},
      {"no-recommends", no_argument, 0, 0},
      {"help", no_argument, 0, 'h'},
      {"debug-solver", no_argument, 0, 0},
      {0, 0, 0, 0}
    };
    specific_options = verify_options;
    _command_help = _(
      "verify (ve) [options]\n"
      "\n"
      "Check whether dependencies of installed packages are satisfied"
      " and repair eventual dependency problems.\n"
      "\n"
      "  Command options:\n"
      "    --no-recommends      Do not install recommended packages, only required.\n"
      "-D, --dry-run            Test the repair, do not actually do anything to the system.\n"
      "-r, --repo <alias|#|URI> Use only specified repositories to install missing packages.\n"
    );
    break;
  }

  case ZypperCommand::INSTALL_NEW_RECOMMENDS_e:
  {
    static struct option options[] = {
      {"dry-run", no_argument, 0, 'D'},
      {"repo", required_argument, 0, 'r'},
      {"debug-solver", no_argument, 0, 0},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = options;
    _command_help = _(
      "install-new-recommends (inr) [options]\n"
      "\n"
      "Install newly added packages recommended by already installed packages."
      " This can typically be used to install new language packages or drivers"
      " for newly added hardware.\n"
      "\n"
      "  Command options:\n"
      "-r, --repo <alias|#|URI> Use only specified repositories to install packages.\n"
      "-D, --dry-run            Test the installation, do not actually install anything.\n"
      "    --debug-solver       Create solver test case for debugging.\n"
    );
    break;
  }

  case ZypperCommand::ADD_REPO_e:
  {
    static struct option service_add_options[] = {
      {"type", required_argument, 0, 't'},
      {"disable", no_argument, 0, 'd'},
      {"disabled", no_argument, 0, 'd'}, // backward compatibility
      {"repo", required_argument, 0, 'r'},
      {"help", no_argument, 0, 'h'},
      {"check", no_argument, 0, 'c'},
      {"no-check", no_argument, 0, 'C'},
      {"name", required_argument, 0, 'n'},
      {"keep-packages", no_argument, 0, 'k'},
      {"no-keep-packages", no_argument, 0, 'K'},
      {0, 0, 0, 0}
    };
    specific_options = service_add_options;
    _command_help = str::form(_(
      // TranslatorExplanation the %s = "yast2, rpm-md, plaindir"
      "addrepo (ar) [options] <URI> <alias>\n"
      "addrepo (ar) [options] <FILE.repo>\n"
      "\n"
      "Add a repository to the sytem. The repository can be specified by its URI"
      " or can be read from specified .repo file (even remote).\n"
      "\n"
      "  Command options:\n"
      "-r, --repo <FILE.repo>  Read the URI and alias from a file (even remote).\n"
      "-t, --type <TYPE>       Type of repository (%s).\n"
      "-d, --disable           Add the repository as disabled.\n"
      "-c, --check             Probe URI.\n"
      "-C, --no-check          Don't probe URI, probe later during refresh.\n"
      "-n, --name              Specify descriptive name for the repository.\n"
      "-k, --keep-packages     Enable RPM files caching.\n"
      "-K, --no-keep-packages  Disable RPM files caching.\n"
    ), "yast2, rpm-md, plaindir");
    break;
  }

  case ZypperCommand::LIST_REPOS_e:
  {
    static struct option service_list_options[] = {
      {"export", required_argument, 0, 'e'},
      {"uri", no_argument, 0, 'u'},
      {"url", no_argument, 0, 'u'},
      {"priority", no_argument, 0, 'p'},
      {"details", no_argument, 0, 'd'},
      {"sort-by-priority", no_argument, 0, 'P'},
      {"sort-by-uri", no_argument, 0, 'U'},
      {"sort-by-alias", no_argument, 0, 'A'},
      {"sort-by-name", no_argument, 0, 'N'},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = service_list_options;

    // handle the conflicting rug's lr here:
    if (_gopts.is_rug_compatible)
    {
      static struct option options[] = {
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
      };
      specific_options = options;

      _command_help = _(
        // translators: this is just a rug compatiblity command
        "list-resolvables (lr)\n"
        "\n"
        "List available resolvable types.\n"
      );
      break;
    }

    _command_help = _(
      "repos (lr) [options]\n"
      "\n"
      "List all defined repositories.\n"
      "\n"
      "  Command options:\n"
      "-e, --export <FILE.repo>  Export all defined repositories as a single local .repo file.\n"
      "-u, --uri                 Show also base URI of repositories.\n"
      "-p, --priority            Show also repository priority.\n"
      "-d, --details             Show more information like URI, priority, type.\n"
      "-U, --sort-by-uri         Sort the list by URI.\n"
      "-P, --sort-by-priority    Sort the list by repository priority.\n"
      "-A, --sort-by-alias       Sort the list by alias.\n"
      "-N, --sort-by-name        Sort the list by name.\n"
    );
    break;
  }

  case ZypperCommand::REMOVE_REPO_e:
  {
    static struct option service_delete_options[] = {
      {"help", no_argument, 0, 'h'},
      {"loose-auth", no_argument, 0, 0},
      {"loose-query", no_argument, 0, 0},
      {0, 0, 0, 0}
    };
    specific_options = service_delete_options;
    _command_help = _(
      "removerepo (rr) [options] <alias|#|URI>\n"
      "\n"
      "Remove repository specified by alias, number or URI.\n"
      "\n"
      "  Command options:\n"
      "    --loose-auth   Ignore user authentication data in the URI.\n"
      "    --loose-query  Ignore query string in the URI.\n"
    );
    break;
  }

  case ZypperCommand::RENAME_REPO_e:
  {
    static struct option service_rename_options[] = {
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = service_rename_options;
    _command_help = _(
      "renamerepo (nr) [options] <alias|#|URI> <new-alias>\n"
      "\n"
      "Assign new alias to the repository specified by alias, number or URI.\n"
      "\n"
      "This command has no additional options.\n"
    );
    break;
  }

  case ZypperCommand::MODIFY_REPO_e:
  {
    static struct option service_modify_options[] = {
      {"help", no_argument, 0, 'h'},
      {"disable", no_argument, 0, 'd'},
      {"enable", no_argument, 0, 'e'},
      {"refresh", no_argument, 0, 'r'},
      {"enable-autorefresh", no_argument, 0, 0 }, // backward compatibility
      {"no-refresh", no_argument, 0, 'R'},
      {"disable-autorefresh", no_argument, 0, 0 }, // backward compatibility
      {"name", required_argument, 0, 'n'},
      {"priority", required_argument, 0, 'p'},
      {"keep-packages", no_argument, 0, 'k'},
      {"no-keep-packages", no_argument, 0, 'K'},
      {"all", no_argument, 0, 'a' },
      {"local", no_argument, 0, 'l' },
      {"remote", no_argument, 0, 't' },
      {"medium-type", required_argument, 0, 'm' },
      {0, 0, 0, 0}
    };
    specific_options = service_modify_options;
    _command_help = str::form(_(
      // translators: %s is "--all|--remote|--local|--medium-type"
      // and "--all, --remote, --local, --medium-type" 
      "modifyrepo (mr) <options> <alias|#|URI>\n"
      "modifyrepo (mr) <options> <%s>\n"
      "\n"
      "Modify properties of repositories specified by alias, number or URI or by"
      " the '%s' aggregate options.\n"
      "\n"
      "  Command options:\n"
      "-d, --disable             Disable the repository (but don't remove it).\n"
      "-e, --enable              Enable a disabled repository.\n"
      "-r, --refresh             Enable auto-refresh of the repository.\n"
      "-R, --no-refresh          Disable auto-refresh of the repository.\n"
      "-n, --name                Set a descriptive name for the repository.\n"
      "-p, --priority <1-99>     Set priority of the repository.\n"
      "-k, --keep-packages       Enable RPM files caching.\n"
      "-K, --no-keep-packages    Disable RPM files caching.\n"
      "-a, --all                 Apply changes to all repositories.\n"
      "-l, --local               Apply changes to all local repositories.\n"
      "-t, --remote              Apply changes to all remote repositories.\n"
      "-m, --medium-type <type>  Apply changes to repositories of specified type.\n"
    ), "--all|--remote|--local|--medium-type"
     , "--all, --remote, --local, --medium-type");
    break;
  }

  case ZypperCommand::REFRESH_e:
  {
    static struct option refresh_options[] = {
      {"force", no_argument, 0, 'f'},
      {"force-build", no_argument, 0, 'b'},
      {"force-download", no_argument, 0, 'd'},
      {"build-only", no_argument, 0, 'B'},
      {"download-only", no_argument, 0, 'D'},
      {"repo", required_argument, 0, 'r'},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = refresh_options;
    _command_help = _(
      "refresh (ref) [alias|#|URI] ...\n"
      "\n"
      "Refresh repositories specified by their alias, number or URI."
      " If none are specified, all enabled repositories will be refreshed.\n"
      "\n"
      "  Command options:\n"
      "-f, --force              Force a complete refresh.\n"
      "-b, --force-build        Force rebuild of the database.\n"
      "-d, --force-download     Force download of raw metadata.\n"
      "-B, --build-only         Only build the database, don't download metadata.\n"
      "-D, --download-only      Only download raw metadata, don't build the database.\n"
      "-r, --repo <alias|#|URI> Refresh only specified repositories.\n"
    );
    break;
  }

  case ZypperCommand::CLEAN_e:
  {
    static struct option service_list_options[] = {
      {"help", no_argument, 0, 'h'},
      {"repo", required_argument, 0, 'r'},
      {"metadata", no_argument, 0, 'm'},
      {"raw-metadata", no_argument, 0, 'M'},
      {"all", no_argument, 0, 'a'},
      {0, 0, 0, 0}
    };
    specific_options = service_list_options;
    _command_help = _(
      "clean [alias|#|URI] ...\n"
      "\n"
      "Clean local caches.\n"
      "\n"
      "  Command options:\n"
      "-r, --repo <alias|#|URI> Clean only specified repositories.\n"
      "-m, --metadata           Clean metadata cache.\n"
      "-M, --raw-metadata       Clean raw metadata cache.\n"
      "-a, --all                Clean both metadata and package caches.\n"
    );
    break;
  }

  case ZypperCommand::LIST_UPDATES_e:
  {
    static struct option list_updates_options[] = {
      {"repo",        required_argument, 0, 'r'},
      // rug compatibility option, we have --repo
      {"catalog",     required_argument, 0, 'c'},
      {"type",        required_argument, 0, 't'},
      {"best-effort", no_argument, 0, 0 },
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = list_updates_options;
    _command_help = str::form(_(
      // TranslatorExplanation the first %s = "package, patch, pattern, product"
      //  and the second %s = "patch"
      "list-updates (lu) [options]\n"
      "\n"
      "List all available updates.\n"
      "\n"
      "  Command options:\n"
      "-t, --type <type>               Type of resolvable (%s).\n"
      "                                Default: %s.\n"
      "-r, --repo <alias|#|URI>        List only updates from the specified repository.\n"
      "    --best-effort               Do a 'best effort' approach to update. Updates\n"
      "                                to a lower than the latest version are\n"
      "                                also acceptable.\n"
    ), "package, patch, pattern, product", "patch");
    break;
  }

  case ZypperCommand::UPDATE_e:
  {
    static struct option update_options[] = {
      {"repo",                      required_argument, 0, 'r'},
      // rug compatibility option, we have --repo
      {"catalog",                   required_argument, 0, 'c'},
      {"type",                      required_argument, 0, 't'},
      // rug compatibility option, we have global --non-interactive
      // note: rug used this uption only to auto-answer the 'continue with install?' prompt.
      {"no-confirm",                no_argument,       0, 'y'},
      {"skip-interactive",          no_argument,       0, 0},
      {"auto-agree-with-licenses",  no_argument,       0, 'l'},
      // rug compatibility, we have --auto-agree-with-licenses
      {"agree-to-third-party-licenses",  no_argument,  0, 0},
      {"best-effort",               no_argument,       0, 0},
      {"debug-solver",              no_argument,       0, 0},
      {"no-force-resolution",       no_argument,       0, 'R'},
      {"force-resolution",          no_argument,       0,  0 },
      {"no-recommends",             no_argument,       0,  0 },
      {"dry-run",                   no_argument,       0, 'D'},
      // rug uses -N shorthand
      {"dry-run",                   no_argument,       0, 'N'},
      // dummy for now
      {"download-only",             no_argument,       0, 'd'},
      // rug-compatibility - dummy for now
      {"category",                  no_argument,       0, 'g'},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = update_options;
    _command_help = str::form(_(
      // TranslatorExplanation the first %s = "package, patch, pattern, product"
      //  and the second %s = "patch"
      "update (up) [options]\n"
      "\n"
      "Update all installed resolvables with newer versions, where applicable.\n"
      "\n"
      "  Command options:\n"
      "\n"
      "-t, --type <type>               Type of resolvable (%s).\n"
      "                                Default: %s.\n"
      "-r, --repo <alias|#|URI>        Limit updates to the specified repository.\n"
      "    --skip-interactive          Skip interactive updates.\n"
      "-l, --auto-agree-with-licenses  Automatically say 'yes' to third party license confirmation prompt.\n"
      "                                See man zypper for more details.\n"
      "    --best-effort               Do a 'best effort' approach to update. Updates\n"
      "                                to a lower than the latest version are\n"
      "                                also acceptable.\n"
      "    --debug-solver              Create solver test case for debugging.\n"
      "    --no-recommends             Do not install recommended packages, only required.\n"
      "-R, --no-force-resolution       Do not force the solver to find solution, let it ask.\n"
      "    --force-resolution          Force the solver to find a solution (even an agressive).\n"
      "-D, --dry-run                   Test the update, do not actually update.\n"
    ), "package, patch, pattern, product", "patch");
    break;
  }

  case ZypperCommand::DIST_UPGRADE_e:
  {
    static struct option dupdate_options[] = {
      {"repo",                      required_argument, 0, 'r'},
      {"no-recommends",             no_argument,       0,  0 },
      {"auto-agree-with-licenses",  no_argument,       0, 'l'},
      {"debug-solver",              no_argument,       0, 0},
      {"dry-run",                   no_argument,       0, 'D'},
      // rug uses -N shorthand
      {"dry-run",                   no_argument,       0, 'N'},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = dupdate_options;
    _command_help = _(
      "dist-upgrade (dup) [options]\n"
      "\n"
      "Perform a distribution upgrade.\n"
      "\n"
      "  Command options:\n"
      "\n"
      "-r, --repo <alias|#|URI>        Limit the upgrade to the specified repository.\n"
      "    --no-recommends             Do not install recommended packages, only required.\n"
      "-l, --auto-agree-with-licenses  Automatically say 'yes' to third party license confirmation prompt.\n"
      "                                See man zypper for more details.\n"
      "    --debug-solver              Create solver test case for debugging\n"
      "-D, --dry-run                   Test the upgrade, do not actually upgrade\n"
    );
    break;
  }

  case ZypperCommand::SEARCH_e:
  {
    static struct option search_options[] = {
      {"installed-only", no_argument, 0, 'i'},
      {"uninstalled-only", no_argument, 0, 'u'},
      {"match-all", no_argument, 0, 0},
      {"match-any", no_argument, 0, 0},
      {"match-substrings", no_argument, 0, 0},
      {"match-words", no_argument, 0, 0},
      {"match-exact", no_argument, 0, 0},
      {"search-descriptions", no_argument, 0, 'd'},
      {"case-sensitive", no_argument, 0, 'C'},
      {"type",    required_argument, 0, 't'},
      {"sort-by-name", no_argument, 0, 0},
      // rug compatibility option, we have --sort-by-repo
      {"sort-by-catalog", no_argument, 0, 0},
      {"sort-by-repo", no_argument, 0, 0},
      // rug compatibility option, we have --repo
      {"catalog", required_argument, 0, 'c'},
      {"repo", required_argument, 0, 'r'},
      {"details", no_argument, 0, 's'},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = search_options;
    _command_help = _(
      //! \todo add '-s' to --details after 11.0 is out
      "search (se) [options] [querystring] ...\n"
      "\n"
      "Search for packages matching given search strings.\n"
      "\n"
      "  Command options:\n"
      "    --match-all            Search for a match with all search strings (default).\n"
      "    --match-any            Search for a match with any of the search strings.\n"
      "    --match-substrings     Search for a match to partial words (default).\n"
      "    --match-words          Search for a match to whole words only.\n"
      "    --match-exact          Searches for an exact package name.\n"
      "-d, --search-descriptions  Search also in package summaries and descriptions.\n"
      "-C, --case-sensitive       Perform case-sensitive search.\n"
      "-i, --installed-only       Show only packages that are already installed.\n"
      "-u, --uninstalled-only     Show only packages that are not currently installed.\n"
      "-t, --type <type>          Search only for packages of the specified type.\n"
      "-r, --repo <alias|#|URI>   Search only in the specified repository.\n"
      "    --sort-by-name         Sort packages by name (default).\n"
      "    --sort-by-repo         Sort packages by repository.\n"
      "-s, --details              Show each available version in each repository\n"
      "                           on a separate line.\n"
      "\n"
      "* and ? wildcards can also be used within search strings.\n"
    );
    break;
  }

  case ZypperCommand::PATCH_CHECK_e:
  {
    static struct option patch_check_options[] = {
      {"repo", required_argument, 0, 'r'},
      // rug compatibility option, we have --repo
      {"catalog", required_argument, 0, 'c'},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = patch_check_options;
    _command_help = _(
      "patch-check (pchk) [options]\n"
      "\n"
      "Check for available patches.\n"
      "\n"
      "  Command options:\n"
      "\n"
      "-r, --repo <alias|#|URI>  Check for patches only in the specified repository.\n"
    );
    break;
  }

  case ZypperCommand::PATCHES_e:
  {
    static struct option patches_options[] = {
      {"repo", required_argument, 0, 'r'},
      // rug compatibility option, we have --repo
      {"catalog", required_argument, 0, 'c'},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = patches_options;
    _command_help = _(
      "patches (pch) [repository] ...\n"
      "\n"
      "List all patches available in specified repositories.\n"
      "\n"
      "  Command options:\n"
      "\n"
      "-r, --repo <alias|#|URI>  Just another means to specify repository.\n"
    );
    break;
  }

  case ZypperCommand::PACKAGES_e:
  {
    static struct option options[] = {
      {"repo", required_argument, 0, 'r'},
      // rug compatibility option, we have --repo
      {"catalog", required_argument, 0, 'c'},
      {"installed-only", no_argument, 0, 'i'},
      {"uninstalled-only", no_argument, 0, 'u'},
      {"sort-by-name", no_argument, 0, 'N'},
      {"sort-by-repo", no_argument, 0, 'R'},
      {"sort-by-catalog", no_argument, 0, 0},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = options;
    _command_help = _(
      "packages (pa) [options] [repository] ...\n"
      "\n"
      "List all packages available in specified repositories.\n"
      "\n"
      "  Command options:\n"
      "\n"
      "-r, --repo <alias|#|URI>  Just another means to specify repository.\n"
      "-i, --installed-only      Show only installed packages.\n"
      "-u, --uninstalled-only    Show only packages which are not installed.\n"
      "-N, --sort-by-name        Sort the list by package name.\n"
      "-R, --sort-by-repo        Sort the list by repository.\n"
    );
    break;
  }

  case ZypperCommand::PATTERNS_e:
  {
    static struct option options[] = {
      {"repo", required_argument, 0, 'r'},
      // rug compatibility option, we have --repo
      {"catalog", required_argument, 0, 'c'},
      {"installed-only", no_argument, 0, 'i'},
      {"uninstalled-only", no_argument, 0, 'u'},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = options;
    _command_help = _(
      "patterns (pt) [options] [repository] ...\n"
      "\n"
      "List all patterns available in specified repositories.\n"
      "\n"
      "  Command options:\n"
      "\n"
      "-r, --repo <alias|#|URI>  Just another means to specify repository.\n"
      "-i, --installed-only      Show only installed patterns.\n"
      "-u, --uninstalled-only    Show only patterns which are not installed.\n"
    );
    break;
  }

  case ZypperCommand::PRODUCTS_e:
  {
    static struct option options[] = {
      {"repo", required_argument, 0, 'r'},
      // rug compatibility option, we have --repo
      {"catalog", required_argument, 0, 'c'},
      {"installed-only", no_argument, 0, 'i'},
      {"uninstalled-only", no_argument, 0, 'u'},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = options;
    _command_help = _(
      "products (pd) [options] [repository] ...\n"
      "\n"
      "List all products available in specified repositories.\n"
      "\n"
      "  Command options:\n"
      "\n"
      "-r, --repo <alias|#|URI>  Just another means to specify repository.\n"
      "-i, --installed-only      Show only installed products.\n"
      "-u, --uninstalled-only    Show only products which are not installed.\n"
    );
    break;
  }

  case ZypperCommand::INFO_e:
  {
    static struct option info_options[] = {
      {"type", required_argument, 0, 't'},
      {"repo", required_argument, 0, 'r'},
      // rug compatibility option, we have --repo
      {"catalog", required_argument, 0, 'c'},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = info_options;
    _command_help = str::form(_(
        "info (if) [options] <name> ...\n"
        "\n"
        "Show detailed information for specified packages.\n"
        "\n"
        "  Command options:\n"
        "-r, --repo <alias|#|URI>  Work only with the specified repository.\n"
        "-t, --type <type>         Type of resolvable (%s).\n"
        "                          Default: %s."
      ), "package, patch, pattern, product", "package");

    break;
  }

  // rug compatibility command, we have zypper info [-t <res_type>]
  case ZypperCommand::RUG_PATCH_INFO_e:
  {
    static struct option patch_info_options[] = {
      {"catalog", required_argument, 0, 'c'},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = patch_info_options;
    _command_help = str::form(_(
      "patch-info <patchname> ...\n"
      "\n"
      "Show detailed information for patches.\n"
      "\n"
      "This is a rug compatibility alias for '%s'.\n"
    ), "zypper info -t patch");
    break;
  }

  // rug compatibility command, we have zypper info [-t <res_type>]
  case ZypperCommand::RUG_PATTERN_INFO_e:
  {
    static struct option options[] = {
      {"catalog", required_argument, 0, 'c'},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = options;
    _command_help = str::form(_(
      "pattern-info <pattern_name> ...\n"
      "\n"
      "Show detailed information for patterns.\n"
      "\n"
      "This is a rug compatibility alias for '%s'.\n"
    ), "zypper info -t pattern");
    break;
  }

  // rug compatibility command, we have zypper info [-t <res_type>]
  case ZypperCommand::RUG_PRODUCT_INFO_e:
  {
    static struct option options[] = {
      {"catalog", required_argument, 0, 'c'},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = options;
    _command_help = str::form(_(
      "product-info <product_name> ...\n"
      "\n"
      "Show detailed information for products.\n"
      "\n"
      "This is a rug compatibility alias for '%s'.\n"
    ), "zypper info -t product");
    break;
  }

  case ZypperCommand::WHAT_PROVIDES_e:
  {
    static struct option options[] = {
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = options;
    _command_help = _(
      "what-provides (wp) <capability>\n"
      "\n"
      "List all packages providing the specified capability.\n"
      "\n"
      "This command has no additional options.\n"
    );
    break;
  }
/*
  case ZypperCommand::WHAT_REQUIRES_e:
  {
    static struct option options[] = {
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = options;
    _command_help = _(
      "what-requires (wr) <capability>\n"
      "\n"
      "List all packages requiring the specified capability.\n"
      "\n"
      "This command has no additional options.\n"
    );
    break;
  }

  case ZypperCommand::WHAT_CONFLICTS_e:
  {
    static struct option options[] = {
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = options;
    _command_help = _(
      "what-conflicts (wc) <capability>\n"
      "\n"
      "List all packages conflicting with the specified capability.\n"
      "\n"
      "This command has no additional options.\n"
    );
    break;
  }
*/
  case ZypperCommand::MOO_e:
  {
    static struct option moo_options[] = {
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = moo_options;
    _command_help = _(
      "moo\n"
      "\n"
      "Show an animal.\n"
      "\n"
      "This command has no additional options.\n"
      );
    break;
  }

  case ZypperCommand::ADD_LOCK_e:
  {
    static struct option options[] =
    {
      {"type", required_argument, 0, 't'},
      {"repo", required_argument, 0, 'r'},
      // rug compatiblity (although rug does not seem to support this)
      {"catalog", required_argument, 0, 'c'},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = options;
    _command_help = str::form(_(
      "addlock (al) [options] <packagename>\n"
      "\n"
      "Add a package lock. Specify packages to lock by exact name or by a"
      " glob pattern using '*' and '?' wildcard characters.\n"
      "\n"
      "  Command options:\n"
      "-r, --repo <alias|#|URI>  Restrict the lock to the specified repository.\n"
      "-t, --type <type>         Type of resolvable (%s).\n"
      "                          Default: %s.\n"
    ), "package, patch, pattern, product", "package");

    break;
  }

  case ZypperCommand::REMOVE_LOCK_e:
  {
    static struct option options[] =
    {
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = options;
    _command_help = _(
      "removelock (rl) <lock-number>\n"
      "\n"
      "Remove a package lock. Specify the lock to remove by its number obtained"
      " with 'zypper locks'.\n"
      "\n"
      "This command has no additional options.\n"
    );
    break;
  }

  case ZypperCommand::LIST_LOCKS_e:
  {
    static struct option options[] =
    {
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = options;
    _command_help = _(
      "locks (ll)\n"
      "\n"
      "List current package locks.\n"
      "\n"
      "This command has no additional options.\n"
    );
    break;
  }

  case ZypperCommand::CLEAN_LOCKS_e:
  {
    static struct option options[] =
    {
      {"help", no_argument, 0, 'h'},
      {"only-duplicates", no_argument, 0, 'd' },
      {"only-empty", no_argument, 0, 'e' },
      {0, 0, 0, 0}
    };
    specific_options = options;
    _command_help = ( //TODO localize
      "clean locks (ll)\n"
      "\n"
      "Removes useless locks. Before removing locks which doesn't lock anything, ask user.\n"
      "\n"
      "  Command options:\n"
      "-d, --only-duplicates     Clean only duplicate locks.\n"
      "-e, --only-empty          Clean only locks which doesn't lock anything.\n"
    );
    break;
  }

  case ZypperCommand::SHELL_QUIT_e:
  {
    static struct option quit_options[] = {
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = quit_options;
    _command_help = _(
      "quit (exit, ^D)\n"
      "\n"
      "Quit the current zypper shell.\n"
      "\n"
      "This command has no additional options.\n"
    );
    break;
  }

  case ZypperCommand::SHELL_e:
  {
    static struct option quit_options[] = {
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = quit_options;
    _command_help = _(
      "shell (sh)\n"
      "\n"
      "Enter the zypper command shell.\n"
      "\n"
      "This command has no additional options.\n"
    );
    break;
  }

  case ZypperCommand::RUG_SERVICE_TYPES_e:
  {
    static struct option options[] = {
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = options;
    _command_help = _(
      // translators: this is just a rug-compatiblity command
      "service-types (st)\n"
      "\n"
      "List available service types.\n"
    );
    break;
  }

  case ZypperCommand::RUG_LIST_RESOLVABLES_e:
  {
    static struct option options[] = {
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = options;
    _command_help = _(
      // translators: this is just a rug-compatiblity command
      "list-resolvables (lr)\n"
      "\n"
      "List available resolvable types.\n"
    );
    break;
  }

  case ZypperCommand::RUG_MOUNT_e:
  {
    static struct option options[] = {
      {"alias", required_argument, 0, 'a'},
      {"name", required_argument, 0, 'n'},
      // dummy for now - always recurse
      {"recurse", required_argument, 0, 'r'},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = options;
    _command_help = _(
      // trunslators: this is a rug-compatibility command (equivalent of
      // 'zypper addrepo -t plaindir URI'). You can refer to rug's translations
      // for how to translate specific terms like channel or service if in doubt.
      "mount\n"
      "\n"
      "Mount directory with RPMs as a channel.\n"
      "\n"
      "  Command options:\n"
      "-a, --alias <alias>  Use given string as service alias.\n"
      "-n, --name <name>    Use given string as service name.\n"
      "-r, --recurse        Dive into subdirectories.\n"
    );
    break;
  }

  case ZypperCommand::RUG_PATCH_SEARCH_e:
  {
    static struct option search_options[] = {
      {"installed-only", no_argument, 0, 'i'},
      {"uninstalled-only", no_argument, 0, 'u'},
      {"match-all", no_argument, 0, 0},
      {"match-any", no_argument, 0, 0},
      {"match-substrings", no_argument, 0, 0},
      {"match-words", no_argument, 0, 0},
      {"match-exact", no_argument, 0, 0},
      {"search-descriptions", no_argument, 0, 'd'},
      {"case-sensitive", no_argument, 0, 'C'},
      {"sort-by-name", no_argument, 0, 0},
      {"sort-by-catalog", no_argument, 0, 0},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}
    };
    specific_options = search_options;
    _command_help = str::form(_(
      "patch-search [options] [querystring...]\n"
      "\n"
      "Search for patches matching given search strings. This is a"
      " rug-compatibility alias for '%s'. See zypper's manual page for details.\n"
    ), "zypper -r search -t patch --detail ...");
    break;
  }

  default:
  {
    if (runningHelp())
      break;

    ERR << "Unknown or unexpected command" << endl;
    out().error(_("Unexpected program flow."));
    report_a_bug(out());
  }
  }

  // parse command options
  if (!runningHelp())
  {
    ::copts = _copts = parse_options (argc(), argv(), specific_options);
    if (copts.count("_unknown"))
    {
      setExitCode(ZYPPER_EXIT_ERR_SYNTAX);
      ERR << "Unknown option, returning." << endl;
      return;
    }

    MIL << "Done parsing options." << endl;

    // treat --help command option like global --help option from now on
    // i.e. when used together with command to print command specific help
    setRunningHelp(runningHelp() || copts.count("help"));

    if (optind < argc()) {
      ostringstream s;
      s << _("Non-option program arguments: ");
      while (optind < argc()) {
        string argument = argv()[optind++];
        s << "'" << argument << "' ";
        _arguments.push_back (argument);
      }
      out().info(s.str(), Out::HIGH);
    }

    // here come commands that need the lock
    try
    {
      const char *roh = getenv("ZYPP_READONLY_HACK");
      if (roh != NULL && roh[0] == '1')
        zypp_readonly_hack::IWantIt ();
      else if (command() == ZypperCommand::LIST_REPOS)
        zypp_readonly_hack::IWantIt (); // #247001, #302152

      God = zypp::getZYpp();
    }
    catch (ZYppFactoryException & excpt_r)
    {
      ZYPP_CAUGHT (excpt_r);
      ERR  << "A ZYpp transaction is already in progress." << endl;
      out().error(
        _("A ZYpp transaction is already in progress."
          " This means, there is another application using the libzypp library for"
          " package management running. All such applications must be closed before"
          " using this command."));

      setExitCode(ZYPPER_EXIT_ERR_ZYPP);
      throw (ExitRequestException("ZYpp locked"));
    }
    catch (Exception & excpt_r)
    {
      ZYPP_CAUGHT (excpt_r);
      out().error(excpt_r.msg());
      setExitCode(ZYPPER_EXIT_ERR_ZYPP);
      throw (ExitRequestException("ZYpp locked"));
    }
  }

  MIL << "Done " << endl;
}

/// process one command from the OS shell or the zypper shell
void Zypper::doCommand()
{
  MIL << "Going to process command " << command().toEnum() << endl;
  ResObject::Kind kind;


  // === execute command ===
  
  switch(command().toEnum())
  {

  // --------------------------( moo )----------------------------------------

  case ZypperCommand::MOO_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }

    // TranslatorExplanation this is a hedgehog, paint another animal, if you want
    out().info(_("   \\\\\\\\\\\n  \\\\\\\\\\\\\\__o\n__\\\\\\\\\\\\\\'/_"));
    break;
  }

  // --------------------------( repo list )----------------------------------
  
  case ZypperCommand::LIST_REPOS_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }

    if (_gopts.is_rug_compatible)
      rug_list_resolvables(*this);
    else
      list_repos(*this);

    break;
  }

  // --------------------------( addrepo )------------------------------------

  case ZypperCommand::ADD_REPO_e:
  case ZypperCommand::RUG_MOUNT_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }

    // check root user
    if (geteuid() != 0 && !globalOpts().changedRoot)
    {
      out().error(
        _("Root privileges are required for modifying system repositories."));
      setExitCode(ZYPPER_EXIT_ERR_PRIVILEGES);
      return;
    }

    // too many arguments
    if (_arguments.size() > 2)
    {
      report_too_many_arguments(_command_help);
      setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
      return;
    }

    // indeterminate indicates the user has not specified the values
    tribool enabled(indeterminate); 

    if (copts.count("disable") || copts.count("disabled"))
      enabled = false;

    tribool keep_pkgs;
    if (copts.count("keep-packages"))
      keep_pkgs = true;
    else if (copts.count("no-keep-packages"))
      keep_pkgs = false;

    try
    {
      // add repository specified in .repo file
      if (copts.count("repo"))
      {
        add_repo_from_file(*this,copts["repo"].front(), enabled, false, keep_pkgs);
        return;
      }
  
      // force specific repository type. Validation is done in add_repo_by_url()
      string type = copts.count("type") ? copts["type"].front() : "";
      if (command() == ZypperCommand::RUG_MOUNT || type == "mount")
        type = "plaindir";
      else if (type == "zypp")
        type = "";

      switch (_arguments.size())
      {
      // display help message if insufficient info was given
      case 0:
        out().error(_("Too few arguments."));
        ERR << "Too few arguments." << endl;
        out().info(_command_help);
        setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
        return;
      case 1:
        if (command() == ZypperCommand::RUG_MOUNT)
        {
          string alias;
          parsed_opts::const_iterator it = _copts.find("alias");
          if (it != _copts.end())
            alias = it->second.front();
          // get the last component of the path
          if (alias.empty())
          {
            Pathname path(_arguments[0]);
            alias = path.basename();
          }
          _arguments.push_back(alias);
          // continue to case 2:
        }
        else if( !isRepoFile(_arguments[0] ))
        {
          out().error(
            _("If only one argument is used, it must be a URI pointing to a .repo file."));
          ERR << "Not a repo file." << endl;
          out().info(_command_help);
          setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
          return;
        }
        else
        {
          add_repo_from_file(*this,_arguments[0], enabled, false, keep_pkgs);
          break;
        }
      case 2:
	Url url = make_url (_arguments[0]);
        if (!url.isValid())
        {
          setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
          return;
        }

        // by default, enable the repo and set autorefresh to false
        if (indeterminate(enabled)) enabled = true;
        if (copts.count("check"))
        {
          if (!copts.count("no-check"))
            this->_gopts.rm_options.probe = true;
          else
            this->out().warning(str::form(
              _("Cannot use %s together with %s. Using the %s setting."),
              "--check", "--no-check", "zypp.conf")
                ,Out::QUIET);
        }
        else if (copts.count("no-check"))
          this->_gopts.rm_options.probe = false;

        warn_if_zmd();

        // load gpg keys
        init_target(*this);

        add_repo_by_url(
	    *this, url, _arguments[1]/*alias*/, type, enabled, false, keep_pkgs);
        return;
      }
    }
    catch (const repo::RepoUnknownTypeException & e)
    {
      ZYPP_CAUGHT(e);
      out().error(e,
          _("Specified type is not a valid repository type:"),
          _("See 'zypper -h addrepo' or man zypper to get a list of known repository types."));
      setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
    }

    break;
  }

  // --------------------------( delete repo )--------------------------------

  case ZypperCommand::REMOVE_REPO_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }

    // check root user
    if (geteuid() != 0 && !globalOpts().changedRoot)
    {
      out().error(
        _("Root privileges are required for modifying system repositories."));
      setExitCode(ZYPPER_EXIT_ERR_PRIVILEGES);
      return;
    }

    if (_arguments.size() < 1)
    {
      out().error(_("Required argument missing."));
      ERR << "Required argument missing." << endl;
      ostringstream s;
      s << _("Usage") << ':' << endl;
      s << _command_help;
      out().info(s.str());
      setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
      return;
    }

    warn_if_zmd ();

    //must store repository before remove to ensure correct match number
    set<RepoInfo,RepoInfoAliasComparator> repo_to_remove;
    for (vector<string>::const_iterator it = _arguments.begin();
	it!= _arguments.end();++it){
      RepoInfo repo;
      if (match_repo(*this,*it,&repo))
      {
	repo_to_remove.insert(repo);
      }
      else
      {
	MIL << "Repository not found by given alias, number or URI." << endl;
	out().error(boost::str(format(
	  //TranslatorExplanation %s is string which was not found (can be url,
	  //alias or the repo number)
	  _("Repository %s not found by alias, number or URI."))
	    % *it));
      }
    }

    for_ (it,repo_to_remove.begin(),repo_to_remove.end())
      remove_repo(*this,*it);

    break;
  }

  // --------------------------( rename repo )--------------------------------

  case ZypperCommand::RENAME_REPO_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }

    // check root user
    if (geteuid() != 0 && !globalOpts().changedRoot)
    {
      out().error(
        _("Root privileges are required for modifying system repositories."));
      setExitCode(ZYPPER_EXIT_ERR_PRIVILEGES);
      return;
    }

    if (_arguments.size() < 2)
    {
      out().error(_("Too few arguments. At least URI and alias are required."));
      ERR << "Too few arguments. At least URI and alias are required." << endl;
      out().info(_command_help);
      setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
      return;
    }
    // too many arguments
    else if (_arguments.size() > 2)
    {
      report_too_many_arguments(_command_help);
      setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
      return;
    }

//    init_target(*this);
    warn_if_zmd ();
    try {
      RepoInfo repo;
      if (match_repo(*this,_arguments[0], &repo))
      {
	rename_repo(*this, repo.alias(), _arguments[1]);
      }
      else
      {
	 out().error(boost::str(format(
           _("Repository '%s' not found.")) % _arguments[0]));
         ERR << "Repo " << _arguments[0] << " not found" << endl;
      }
    }
    catch ( const Exception & excpt_r )
    {
      out().error(excpt_r.asUserString());
      setExitCode(ZYPPER_EXIT_ERR_ZYPP);
      return;
    }

    return;
  }

  // --------------------------( modify repo )--------------------------------

  case ZypperCommand::MODIFY_REPO_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }

    // check root user
    if (geteuid() != 0 && !globalOpts().changedRoot)
    {
      out().error(
        _("Root privileges are required for modifying system repositories."));
      setExitCode(ZYPPER_EXIT_ERR_PRIVILEGES);
      return;
    }

    //! \todo drop before 11.1
    if (_copts.count("enable-autorefresh"))
      out().warning(str::form(_("'%s' option is deprecated and will be dropped soon."), "enable-autorefresh"));
    if (_copts.count("disable-autorefresh"))
      out().warning(str::form(_("'%s' option is deprecated and will be dropped soon."), "disable-autorefresh"));

    bool non_alias = copts.count("all") || copts.count("local") || 
        copts.count("remote") || copts.count("medium-type");

    if (_arguments.size() < 1 && !non_alias)
    {
      // translators: aggregate option is e.g. "--all". This message will be
      // followed by mr command help text which will explain it
      out().error(_("Alias or an aggregate option is required."));
      ERR << "No alias argument given." << endl;
      out().info(_command_help);
      setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
      return;
    }
    // too many arguments
    if (_arguments.size() > 1
       || (_arguments.size() > 0 && non_alias))
    {
      report_too_many_arguments(_command_help);
      setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
      return;
    }
    
    if (non_alias)
    {
      modify_repos_by_option(*this);
    }
    else
    {
      RepoInfo repo;
      if (match_repo(*this,_arguments[0],&repo))
      {
        modify_repo(*this, repo.alias());
      }
      else 
      {
        out().error(
          boost::str(format(_("Repository %s not found.")) % _arguments[0]));
        ERR << "Repo " << _arguments[0] << " not found" << endl;
      }
    }
    
    break;
  }

  // --------------------------( refresh )------------------------------------

  case ZypperCommand::REFRESH_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }

    // check root user
    if (geteuid() != 0 && !globalOpts().changedRoot)
    {
      out().error(
        _("Root privileges are required for refreshing system repositories."));
      setExitCode(ZYPPER_EXIT_ERR_PRIVILEGES);
      return;
    }

    if (globalOpts().no_refresh)
      out().warning(boost::str(format(
        _("The '%s' global option has no effect here."))
        % "--no-refresh"));

    refresh_repos(*this);
    break;
  }

  // --------------------------( clean )------------------------------------

  case ZypperCommand::CLEAN_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }

    // check root user
    if (geteuid() != 0 && !globalOpts().changedRoot)
    {
      out().error(
        _("Root privileges are required for cleaning local caches."));
      setExitCode(ZYPPER_EXIT_ERR_PRIVILEGES);
      return;
    }
    
    clean_repos(*this);
    break;
  }

  // --------------------------( remove/install )-----------------------------

  case ZypperCommand::INSTALL_e:
  case ZypperCommand::REMOVE_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }

    if (_arguments.size() < 1)
    {
      out().error(string(_("Too few arguments.")) + " " +
          _("At least one package name is required.") + "\n");
      out().info(_command_help);
      setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
      return;
    }

    if (copts.count("auto-agree-with-licenses")
        || copts.count("agree-to-third-party-licenses"))
      _cmdopts.license_auto_agree = true;

    // check root user
    if (geteuid() != 0 && !globalOpts().changedRoot)
    {
      out().error(
        _("Root privileges are required for installing or uninstalling packages."));
      setExitCode(ZYPPER_EXIT_ERR_PRIVILEGES);
      return;
    }

    // rug compatibility code
    // switch on non-interactive mode if no-confirm specified
    if (copts.count("no-confirm"))
      _gopts.non_interactive = true;


    // read resolvable type
    string skind = copts.count("type")?  copts["type"].front() : "package";
    kind = string_to_kind (skind);
    if (kind == ResObject::Kind ()) {
      out().error(boost::str(format(_("Unknown resolvable type: %s")) % skind));
      setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
      return;
    }

    bool install_not_remove = command() == ZypperCommand::INSTALL;

    // check for rpm files among the arguments
    ArgList rpms_files_caps;
    if (install_not_remove)
    {
      for (vector<string>::iterator it = _arguments.begin();
            it != _arguments.end(); )
      {
        if (looks_like_rpm_file(*it))
        {
          DBG << *it << " looks like rpm file" << endl;
          out().info(boost::str(format(
            _("'%s' looks like an RPM file. Will try to download it.")) % *it),
            Out::HIGH);

          // download the rpm into the cache
          //! \todo do we want this or a tmp dir? What about the files cached before?
          //! \todo optimize: don't mount the same media multiple times for each rpm
          Pathname rpmpath = cache_rpm(*it, 
              (_gopts.root_dir != "/" ? _gopts.root_dir : "")
              + ZYPPER_RPM_CACHE_DIR);

          if (rpmpath.empty())
          {
            out().error(boost::str(format(
              _("Problem with the RPM file specified as '%s', skipping."))
              % *it));
          }
          else
          {
            using target::rpm::RpmHeader;
            // rpm header (need name-version-release)
            RpmHeader::constPtr header =
              RpmHeader::readPackage(rpmpath, RpmHeader::NOSIGNATURE);
            if (header)
            {
              string nvrcap =
                header->tag_name() + "=" +
                header->tag_version() + "-" +
                header->tag_release(); 
              DBG << "rpm package capability: " << nvrcap << endl;
  
              // store the rpm file capability string (name=version-release) 
              rpms_files_caps.push_back(nvrcap);
            }
            else
            {
              out().error(boost::str(format(
                _("Problem reading the RPM header of %s. Is it an RPM file?"))
                  % *it));
            }
          }

          // remove this rpm argument 
          it = _arguments.erase(it);
        }
        else
          ++it;
      }
    }

    // if there were some rpm files, add the rpm cache as a temporary plaindir repo 
    if (!rpms_files_caps.empty())
    {
      // add a plaindir repo
      RepoInfo repo;
      repo.setType(repo::RepoType::RPMPLAINDIR);
      repo.addBaseUrl(Url("dir://"
          + (_gopts.root_dir != "/" ? _gopts.root_dir : "")
          + ZYPPER_RPM_CACHE_DIR));
      repo.setEnabled(true);
      repo.setAutorefresh(true);
      repo.setAlias(TMP_RPM_REPO_ALIAS);
      repo.setName(_("Plain RPM files cache"));
      repo.setKeepPackages(false);

      // shut up zypper
      Out::Verbosity tmp = out().verbosity();
      out().setVerbosity(Out::QUIET);

      add_repo(*this, repo);
      refresh_repo(*this, repo);

      out().setVerbosity(tmp);
    }
    // no rpms and no other arguments either
    else if (_arguments.empty())
    {
      out().error(_("No valid arguments specified."));
      setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
      return;
    }

    //! \todo quit here if the argument list remains empty after founding only invalid rpm args

    // prepare repositories
    init_repos(*this);
    if (exitCode() != ZYPPER_EXIT_OK)
      return;

    if ( gData.repos.empty() )
    {
      out().error(_("Warning: No repositories defined."
          " Operating only with the installed resolvables."
          " Nothing can be installed."));
    }

    // prepare target
    init_target(*this);
    // load metadata
    load_resolvables(*this);
    // needed to compute status of PPP
    resolve(*this);

    // tell the solver what we want
    install_remove(*this, _arguments, install_not_remove, kind);
    install_remove(*this, rpms_files_caps, true, kind);

    solve_and_commit(*this);

    break;
  }

  // -------------------( source install )------------------------------------

  case ZypperCommand::SRC_INSTALL_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }

    if (_arguments.size() < 1)
    {
      out().error(_("Source package name is a required argument."));
      setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
      return;
    }

    init_repos(*this);
    if (exitCode() != ZYPPER_EXIT_OK)
      return;
    init_target(*this);
    if (!copts.count("no-build-deps"))
      load_target_resolvables(*this);
    load_repo_resolvables(*this);

    if (!copts.count("no-build-deps"))
      build_deps_install(*this);
    if (!copts.count("build-deps-only"))
      find_src_pkgs(*this);
    solve_and_commit(*this);
    break;
  }

  case ZypperCommand::VERIFY_e:
  case ZypperCommand::INSTALL_NEW_RECOMMENDS_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }

    // too many arguments
    if (_arguments.size() > 0)
    {
      report_too_many_arguments(_command_help);
      setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
      return;
    }

    // rug compatibility code
    // switch on non-interactive mode if no-confirm specified
    if (copts.count("no-confirm"))
      _gopts.non_interactive = true;

    init_repos(*this);
    if (exitCode() != ZYPPER_EXIT_OK)
      return;

    if ( gData.repos.empty() )
    {
      out().error(_("Warning: No repositories defined."
          " Operating only with the installed resolvables."
          " Nothing can be installed."));
    }

    // prepare target
    init_target(*this);
    // load metadata
    load_resolvables(*this);

    solve_and_commit(*this);

    break;
  }

  // --------------------------( search )-------------------------------------

  case ZypperCommand::SEARCH_e:
  case ZypperCommand::RUG_PATCH_SEARCH_e:
  {
    if (command() == ZypperCommand::RUG_PATCH_SEARCH)
      _gopts.is_rug_compatible = true;
    
    zypp::PoolQuery query;

    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }

    if (globalOpts().disable_system_resolvables || copts.count("uninstalled-only"))
      query.setUninstalledOnly(); // beware: this is not all to it, look at zypper-search, _only_not_installed
    if (copts.count("installed-only"))
      query.setInstalledOnly();
    //if (copts.count("match-any")) options.setMatchAny();
    if (copts.count("match-words"))
      query.setMatchWord();
    if (copts.count("match-exact"))
      query.setMatchExact();
    if (copts.count("case-sensitive"))
      query.setCaseSensitive();

    if (command() == ZypperCommand::RUG_PATCH_SEARCH)
      query.addKind(ResKind::patch);
    else if (globalOpts().is_rug_compatible)
      query.addKind(ResKind::package);
    else if (copts.count("type") > 0)
    {
      std::list<std::string>::const_iterator it;
      for (it = copts["type"].begin(); it != copts["type"].end(); ++it)
      {
        kind = string_to_kind( *it );
        if (kind == ResObject::Kind())
        {
          out().error(boost::str(format(
            _("Unknown resolvable type '%s'.")) % *it));
          setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
          return;
        }
        query.addKind( kind );
      }
    }

    init_repos(*this);
    if (exitCode() != ZYPPER_EXIT_OK)
      return;

    // add available repos to query
    if (cOpts().count("repo"))
    {
      std::list<zypp::RepoInfo>::const_iterator repo_it;
      for (repo_it = gData.repos.begin();repo_it != gData.repos.end();++repo_it){
        query.addRepo( repo_it->alias());
      }
    }

    for(vector<string>::const_iterator it = _arguments.begin();
        it != _arguments.end(); ++it)
    {
      query.addString(*it);
      if (!query.matchGlob() && it->find_first_of("?*") != string::npos)
        query.setMatchGlob();
    }
    query.addAttribute(sat::SolvAttr::name);
    if (cOpts().count("search-descriptions"))
    {
      query.addAttribute(sat::SolvAttr::summary);
      query.addAttribute(sat::SolvAttr::description);
    }

    init_target(*this);

    // now load resolvables:
    load_resolvables(*this);
    // needed to compute status of PPP
    resolve(*this);

    Table t;
    t.style(Ascii);

    try
    {
      if (command() == ZypperCommand::RUG_PATCH_SEARCH)
      {
        FillPatchesTable callback(t, query.statusFilterFlags() & PoolQuery::UNINSTALLED_ONLY);
        invokeOnEach(query.poolItemBegin(), query.poolItemEnd(), callback);
      }
      else if (_gopts.is_rug_compatible || _copts.count("details"))
      {
        FillSearchTableSolvable callback(t, query.statusFilterFlags() & PoolQuery::UNINSTALLED_ONLY);
        invokeOnEach(query.selectableBegin(), query.selectableEnd(), callback);
      }
      else
      {
        FillSearchTableSelectable callback(t, query.statusFilterFlags() & PoolQuery::UNINSTALLED_ONLY);
        invokeOnEach(query.selectableBegin(), query.selectableEnd(), callback);
      }

      if (t.empty())
        out().info(_("No resolvables found."), Out::QUIET);
      else
      {
        cout << endl; //! \todo  out().separator()?

        if (command() == ZypperCommand::RUG_PATCH_SEARCH)
        {
          if (copts.count("sort-by-catalog") || copts.count("sort-by-repo"))
            t.sort(1);
          else
            t.sort(3); // sort by name
        }
        else if (_gopts.is_rug_compatible)
        {
          if (copts.count("sort-by-catalog") || copts.count("sort-by-repo"))
            t.sort(1);
          else
            t.sort(3); // sort by name
        }
        else if (_copts.count("details"))
        {
          if (copts.count("sort-by-catalog") || copts.count("sort-by-repo"))
            t.sort(5);
          else
            t.sort(1); // sort by name
        }
        else
        {
          // sort by name (can't sort by repo)
          t.sort(1);
          if (!globalOpts().no_abbrev)
            t.allowAbbrev(2);
        }

        cout << t; //! \todo out().table()?
      }
    }
    catch (const Exception & e)
    {
      out().error(e,
        _("Problem occurred initializing or executing the search query") + string(":"),
        string(_("See the above message for a hint.")) + " " +
          _("Running 'zypper refresh' as root might resolve the problem."));
      setExitCode(ZYPPER_EXIT_ERR_ZYPP);
    }

    break;
  }

  // --------------------------( patch check )--------------------------------

  // TODO: rug summary
  case ZypperCommand::PATCH_CHECK_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }

    // too many arguments
    if (_arguments.size() > 0)
    {
      report_too_many_arguments(_command_help);
      setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
      return;
    }

    init_target(*this);
    init_repos(*this);
    if (exitCode() != ZYPPER_EXIT_OK)
      return;

    // TODO additional_sources
    // TODO warn_no_sources
    // TODO calc token?

    // now load resolvables:
    load_resolvables(*this);
    // needed to compute status of PPP
    resolve(*this);

    patch_check();

    if (gData.security_patches_count > 0)
    {
      setExitCode(ZYPPER_EXIT_INF_SEC_UPDATE_NEEDED);
      return;
    }
    if (gData.patches_count > 0)
    {
      setExitCode(ZYPPER_EXIT_INF_UPDATE_NEEDED);
      return;
    }

    break;
  }

  // --------------------------( misc queries )--------------------------------

  case ZypperCommand::PATCHES_e:
  case ZypperCommand::PATTERNS_e:
  case ZypperCommand::PACKAGES_e:
  case ZypperCommand::PRODUCTS_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }

    init_target(*this);
    init_repos(*this, _arguments);
    if (exitCode() != ZYPPER_EXIT_OK)
      return;
    load_resolvables(*this);
    // needed to compute status of PPP
    resolve(*this);

    switch (command().toEnum())
    {
    case ZypperCommand::PATCHES_e:
      list_patches(*this);
      break;
    case ZypperCommand::PATTERNS_e:
      list_patterns(*this);
      break;
    case ZypperCommand::PACKAGES_e:
      list_packages(*this);
      break;
    case ZypperCommand::PRODUCTS_e:
      list_products(*this);
      break;
    default:;
    }

    break;
  }

  case ZypperCommand::WHAT_PROVIDES_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }
    
    if (_arguments.empty())
    {
      report_required_arg_missing(out(), _command_help);
      setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
      return;
    }
    else if (_arguments.size() > 1)
    {
      report_too_many_arguments(out(), _command_help);
      setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
      return;
    }

    init_target(*this);
    init_repos(*this);
    if (exitCode() != ZYPPER_EXIT_OK)
      return;
    load_resolvables(*this);

    switch (command().toEnum())
    {
    case ZypperCommand::WHAT_PROVIDES_e:
      list_what_provides(*this, _arguments[0]);
      break;
    default:;
    }

    break;
  }

  // --------------------------( list updates )-------------------------------

  case ZypperCommand::LIST_UPDATES_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }

    // too many arguments
    if (_arguments.size() > 0)
    {
      report_too_many_arguments(_command_help);
      setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
      return;
    }

    ResKindSet kinds;
    if (copts.count("type"))
    {
      std::list<std::string>::const_iterator it;
      for (it = copts["type"].begin(); it != copts["type"].end(); ++it)
      {
        kind = string_to_kind( *it );
        if (kind == ResObject::Kind())
        {
          out().error(boost::str(format(
            _("Unknown resolvable type '%s'.")) % *it));
          setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
          return;
        }
        kinds.insert(kind);
      }
    }
    else if (globalOpts().is_rug_compatible)
      kinds.insert(ResTraits<Package>::kind);
    else
      kinds.insert(ResTraits<Patch>::kind);

    bool best_effort = copts.count( "best-effort" ); 

    if (globalOpts().is_rug_compatible && best_effort) {
	best_effort = false;
	// 'rug' is the name of a program and must not be translated
	// 'best-effort' is a program parameter and can not be translated
	out().warning(
	  _("Running as 'rug', can't do 'best-effort' approach to update."));
    }
    init_target(*this);
    init_repos(*this);
    if (exitCode() != ZYPPER_EXIT_OK)
      return;
    load_resolvables(*this);
    resolve(*this);
    
    list_updates(*this, kinds, best_effort);

    break;
  }

  // -----------------------------( update )----------------------------------

  case ZypperCommand::UPDATE_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }

    // check root user
    if (geteuid() != 0 && !globalOpts().changedRoot)
    {
      out().error(
        _("Root privileges are required for updating packages."));
      setExitCode(ZYPPER_EXIT_ERR_PRIVILEGES);
      return;
    }

    if (_copts.count("download-only"))
      report_dummy_option(out(), "download-only");
    if (_copts.count("category"))
      report_dummy_option(out(), "category");

    // rug compatibility code
    // switch on non-interactive mode if no-confirm specified
    if (copts.count("no-confirm"))
      _gopts.non_interactive = true;

    if (copts.count("auto-agree-with-licenses")
        || copts.count("agree-to-third-party-licenses"))
      _cmdopts.license_auto_agree = true;

    ResKindSet kinds;
    if (copts.count("type"))
    {
      std::list<std::string>::const_iterator it;
      for (it = copts["type"].begin(); it != copts["type"].end(); ++it)
      {
        kind = string_to_kind( *it );
        if (kind == ResObject::Kind())
        {
          out().error(boost::str(format(
            _("Unknown resolvable type '%s'.")) % *it));
          setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
          return;
        }
        kinds.insert(kind);
      }
    }
    else if (globalOpts().is_rug_compatible || !_arguments.empty() /* bnc #385990*/)
      kinds.insert(ResKind::package);
    else
      kinds.insert(ResKind::patch);

    bool best_effort = copts.count( "best-effort" ); 
    if (globalOpts().is_rug_compatible && best_effort)
    {
      best_effort = false;
      out().warning(str::form(
        // translators: Running as 'rug', can't do 'best-effort' approach to update.
        _("Running as '%s', cannot do '%s' approach to update."),
        "rug", "best-effort"));
    }

    init_target(*this);

    // rug compatibility - treat arguments as repos
    if (_gopts.is_rug_compatible && !_arguments.empty())
      init_repos(*this, _arguments);
    else
      init_repos(*this);

    if (exitCode() != ZYPPER_EXIT_OK)
      return;
    load_resolvables(*this);
    resolve(*this); // needed to compute status of PPP

    bool skip_interactive =
      copts.count("skip-interactive") || globalOpts().non_interactive;

    mark_updates(*this, kinds, skip_interactive, best_effort);

    solve_and_commit(*this);

    break; 
  }

  // ----------------------------( dist-upgrade )------------------------------

  case ZypperCommand::DIST_UPGRADE_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }

    // check root user
    if (geteuid() != 0 && !globalOpts().changedRoot)
    {
      out().error(
        _("Root privileges are required for performing a distribution upgrade."));
      setExitCode(ZYPPER_EXIT_ERR_PRIVILEGES);
      return;
    }

    // too many arguments
    if (_arguments.size() > 0)
    {
      report_too_many_arguments(_command_help);
      setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
      return;
    }

    if (copts.count("auto-agree-with-licenses"))
      _cmdopts.license_auto_agree = true;

    init_target(*this);
    init_repos(*this);
    if (exitCode() != ZYPPER_EXIT_OK)
      return;
    load_resolvables(*this);

    solve_and_commit(*this);

    break;
  }

  // -----------------------------( info )------------------------------------

  case ZypperCommand::INFO_e:
  case ZypperCommand::RUG_PATCH_INFO_e:
  case ZypperCommand::RUG_PATTERN_INFO_e:
  case ZypperCommand::RUG_PRODUCT_INFO_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }

    if (_arguments.size() < 1)
    {
      out().error(_("Required argument missing."));
      ERR << "Required argument missing." << endl;
      ostringstream s;
      s << _("Usage") << ':' << endl;
      s << _command_help;
      out().info(s.str());
      setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
      return;
    }

    switch (command().toEnum())
    {
    case ZypperCommand::RUG_PATCH_INFO_e:
      kind =  ResTraits<Patch>::kind;
      break;
    case ZypperCommand::RUG_PATTERN_INFO_e:
      kind =  ResTraits<Pattern>::kind;
      break;
    case ZypperCommand::RUG_PRODUCT_INFO_e:
      kind =  ResTraits<Product>::kind;
      break;
    default:
    case ZypperCommand::INFO_e:
      // read resolvable type
      string skind = copts.count("type")?  copts["type"].front() : "package";
      kind = string_to_kind (skind);
      if (kind == ResObject::Kind ()) {
        out().error(boost::str(format(
          _("Unknown resolvable type '%s'.")) % skind));
        setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
        return;
      }
    }

    init_target(*this);
    init_repos(*this);
    if (exitCode() != ZYPPER_EXIT_OK)
      return;
    load_resolvables(*this);
    // needed to compute status of PPP
    resolve(*this);

    printInfo(*this, kind);

    return;
  }

  // -----------------------------( locks )------------------------------------

  case ZypperCommand::ADD_LOCK_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }
    
    // check root user
    if (geteuid() != 0 && !globalOpts().changedRoot)
    {
      out().error(
        _("Root privileges are required for adding of package locks."));
      setExitCode(ZYPPER_EXIT_ERR_PRIVILEGES);
      return;
    }

    // too few arguments
    if (_arguments.empty())
    {
      report_required_arg_missing(out(), _command_help);
      setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
      return;
    }
    // too many arguments
    //TODO rug compatibility
/*    else if (_arguments.size() > 1)
    {
      // rug compatibility
      if (_gopts.is_rug_compatible)
        // translators: 'zypper addlock foo' takes only one argument.
        out().warning(_("Only the first command argument considered. Zypper currently does not support versioned locks."));
      else
      {
        report_too_many_arguments(_command_help);
        setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
        return;
      }
    }*/

    ResKindSet kinds;
    if (copts.count("type"))
    {
      std::list<std::string>::const_iterator it;
      for (it = copts["type"].begin(); it != copts["type"].end(); ++it)
      {
        kind = string_to_kind( *it );
        if (kind == ResObject::Kind())
        {
          out().error(boost::str(format(
            _("Unknown resolvable type '%s'.")) % *it));
          setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
          return;
        }
        kinds.insert(kind);
      }
    }
    else
      kinds.insert(ResKind::package);

    add_locks(*this, _arguments, kinds);

    break;
  }

  case ZypperCommand::REMOVE_LOCK_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }

    // check root user
    if (geteuid() != 0 && !globalOpts().changedRoot)
    {
      out().error(
        _("Root privileges are required for adding of package locks."));
      setExitCode(ZYPPER_EXIT_ERR_PRIVILEGES);
      return;
    }

    if (_arguments.size() == 1)
    {
      report_required_arg_missing(out(), _command_help);
      setExitCode(ZYPPER_EXIT_ERR_INVALID_ARGS);
      return;
    }
    
    init_target(*this);
    init_repos(*this);
    if (exitCode() != ZYPPER_EXIT_OK)
      return;
    load_resolvables(*this);

    remove_locks(*this, _arguments);

    break;
  }

  case ZypperCommand::LIST_LOCKS_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }

    list_locks(*this);

    break;
  }
  
  case ZypperCommand::CLEAN_LOCKS_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }
   
    init_target(*this);
    init_repos(*this);
    if (exitCode() != ZYPPER_EXIT_OK)
      return;
    load_resolvables(*this);

    Locks::instance().read();
    Locks::size_type start = Locks::instance().size();
    if ( !copts.count("only-duplicate") )
      Locks::instance().removeEmpty();
    if ( !copts.count("only-empty") )
      Locks::instance().removeDuplicates();

    Locks::instance().save();

    out().info(str::form("removed locks: %lu", (long unsigned)(start - Locks::instance().size())));
    
    break;
  }

  // -----------------------------( shell )------------------------------------

  case ZypperCommand::SHELL_QUIT_e:
  {
    if (runningHelp())
      out().info(_command_help, Out::QUIET);
    else if (!runningShell())
      out().warning(
        _("This command only makes sense in the zypper shell."), Out::QUIET);
    else
      out().error("oops, you wanted to quit, didn't you?"); // should not happen

    break;
  }

  case ZypperCommand::SHELL_e:
  {
    if (runningHelp())
      out().info(_command_help, Out::QUIET);
    else if (runningShell())
      out().info(_("You already are running zypper's shell."));
    else
    {
      out().error(_("Unexpected program flow."));
      report_a_bug(out());
    }

    break;
  }

  case ZypperCommand::RUG_SERVICE_TYPES_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }
    
    Table t;

    TableHeader th;
    th << _("Alias") << _("Name") << _("Description");
    t << th;

    { TableRow tr; tr << "yum" << "YUM" << "YUM server service"; t << tr; } // rpm-md
    { TableRow tr; tr << "yast" << "YaST2" << "YaST2 repository"; t << tr; }
    { TableRow tr; tr << "zypp" << "ZYPP" << "ZYpp installation repository"; t << tr; }
    { TableRow tr; tr << "mount" << "Mount" << "Mount a directory of RPMs"; t << tr; }
    { TableRow tr; tr << "plaindir" << "Plaindir" << "Mount a directory of RPMs"; t << tr; }

    cout << t;

    break;
  }

  case ZypperCommand::RUG_LIST_RESOLVABLES_e:
  {
    if (runningHelp()) { out().info(_command_help, Out::QUIET); return; }
    rug_list_resolvables(*this);
    break;
  }

  default:
    // if the program reaches this line, something went wrong
    setExitCode(ZYPPER_EXIT_ERR_BUG);
  }
}

void Zypper::cleanup()
{
  MIL << "START" << endl;

  // remove the additional repositories specified by --plus-repo
  for (list<RepoInfo>::const_iterator it = gData.additional_repos.begin();
         it != gData.additional_repos.end(); ++it)
    remove_repo(*this, *it);

  // remove tmprpm cache repo
  for_(it, gData.repos.begin(), gData.repos.end())
    if (it->alias() == TMP_RPM_REPO_ALIAS)
    {
      // shut up zypper
      Out::Verbosity tmp = out().verbosity();
      out().setVerbosity(Out::QUIET);
    
      remove_repo(*this, *it);
    
      out().setVerbosity(tmp);
      break;
    }
}

void rug_list_resolvables(Zypper & zypper)
{
  Table t;

  TableHeader th;
  th << _("Resolvable Type");
  t << th;

  { TableRow tr; tr << "package"; t << tr; }
  { TableRow tr; tr << "patch"; t << tr; }
  { TableRow tr; tr << "pattern"; t << tr; }
  { TableRow tr; tr << "product"; t << tr; }

  cout << t;
}


// Local Variables:
// c-basic-offset: 2
// End: