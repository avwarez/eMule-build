cmake_minimum_required(VERSION 3.20)
project(eMule VERSION 0.72 LANGUAGES CXX RC)

# ---------------------------------------------------------------------------
# This build targets the original MFC/Windows application, as built today by
# srchybrid/emule.vcxproj (MSVC + static MFC/ATL). It is Windows-only.
#
# All third-party dependencies (mbedtls, cryptopp, id3lib, zlib, miniupnpc,
# resizablelib) are vendored as vanilla git submodules - see each
# add_subdirectory() below and the corresponding *_build/CMakeLists.txt for
# the ones with no CMake support of their own.
# ---------------------------------------------------------------------------

if(NOT WIN32)
    message(FATAL_ERROR "eMule (this target) is an MFC/Windows application and can only be built on Windows with MSVC.")
endif()

# ---------------------------------------------------------------------------
# Every dependency here is meant to link statically against the MSVC CRT
# (matching emule.vcxproj's own UseOfMfc=Static philosophy - MSVC_RUNTIME_LIBRARY
# is set to "MultiThreaded$<$<CONFIG:Debug>:Debug>" on every *_build target
# below). zlib, miniupnpc and mbedtls/tf-psa-crypto have their own upstream
# CMakeLists.txt that we don't edit, and none of them set this explicitly, so
# without this they fall back to CMake's own default (dynamic CRT, /MD) -
# confirmed by an actual build attempt: LNK2001 on __imp_close/__imp_read/
# __imp_open/__imp_write/__imp_setbuf/__imp__memicmp (DLL-import CRT thunks
# that only exist when linking the dynamic CRT) from zs.lib/libminiupnpc.lib/
# tfpsacrypto.lib, mixed into an otherwise fully statically-linked emule.exe.
# CMP0091 (default NEW here - cmake_minimum_required is well past 3.15, the
# policy's introduction) makes CMAKE_MSVC_RUNTIME_LIBRARY, set before any
# target is created, the default for every target in every add_subdirectory()
# below unless a target overrides it itself - already the case for cryptopp,
# id3lib, resizablelib and zlibstatic.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# Submodule patching
#
# Every submodule in this repo is vanilla and is never edited in place. When
# one of them needs a source change - an upstream bug, an incompatibility
# with a newer dependency, or a construct the C++ standard we compile at has
# removed - the fix lives as a .patch file in that submodule's *_build/
# directory here, and is applied at configure time by the function below.
#
# Idempotent: `git apply --check --reverse` succeeds exactly when a patch is
# already applied, so re-configuring an existing build tree is a no-op
# instead of a failure.
#
# --ignore-whitespace: these patches are generated against the repository's
# own bytes (LF line endings), but a Windows checkout with core.autocrlf=true
# - the default on GitHub's windows runner images - rewrites the working tree
# to CRLF, and git apply then refuses the LF context. Confirmed by an actual
# reproduction: with a CRLF working tree and core.autocrlf=false, every patch
# here fails with "patch does not apply"; --ignore-whitespace makes all of
# them apply in both cases. The CI workflow additionally pins
# core.autocrlf=false so the tree matches the patches byte for byte. Patches within one directory are applied in sorted
# filename order; give them a numeric prefix if two of them ever have to
# touch the same hunk in a defined sequence.
#
# The numeric prefixes in emule_build/ also group the patches by why they
# exist, which is the first thing you want to know when one of them stops
# applying after a submodule bump:
#   00-89  the code does not compile as-is (a dependency's API changed, or
#          the C++ standard we build at removed the construct).
#   90-94  the code compiles but /Wall flags it; these bring the warning
#          count back to zero.
#   95-96  the code compiles and is warning-clean but is not correct on
#          every target we build, or in every environment we run it in
#          (see each patch's own subject). 95 and 96 are the first kind:
#          constructs that hold on x86 and not on ARM64. 96b is the second,
#          and the distinction it rests on is the whole reason this band is
#          separate from 97 below: the code it changes is correct on
#          Windows - measured, not assumed - and only fails elsewhere.
#
#          96b-webserver-accept-readiness changes how the web interface's
#          listening thread learns that a connection is waiting. It used to
#          arm the socket with WSAEventSelect(FD_ACCEPT), wait on the event
#          with no timeout, and then accept() in a loop until accept()
#          failed. FD_ACCEPT is an edge: delivery disables it and only
#          accept() enables it again, so a single missed delivery leaves the
#          thread in a wait nothing can end - there is no timeout to expire
#          and accept(), the one call that would re-arm it, is never
#          reached. Nothing creates that thread a second time, so the web
#          interface then stays silent for the rest of the run while
#          connections collect in the backlog; on the machine this was found
#          on, seventeen of them were still sitting there two days later.
#          The patch watches the socket with select() instead, which reports
#          the socket's present state rather than a transition, and takes
#          one connection per report instead of draining: there is no edge
#          to miss, nothing to re-arm, and accept() is no longer called with
#          nothing waiting. The select() timeout exists only so the thread
#          notices the terminate event without a second object to wait on;
#          it is not what makes the loop correct.
#
#          Taking one connection per report has a corner the drain loop did
#          not have, and the patch closes it. An accept() that fails after
#          select() said readable is ordinary when it fails with
#          WSAEWOULDBLOCK - another taker, or a peer that gave up in
#          between - and the next pass waits in select() as before. Any
#          other error left the connection that made the socket readable
#          where it was, so select() reports it again at once and going
#          straight back to the top would spin the thread for as long as the
#          condition lasts; running out of descriptors is the case that
#          matters, and waiting is the only useful answer to it. The patch
#          therefore waits a select() period on any error that is not
#          WSAEWOULDBLOCK, which costs what an idle pass already costs, and
#          writes one log line for the spell rather than one per pass - the
#          error code has to change, or a minute to pass, before it says so
#          again. That line ships: it is an error report on a path that
#          should never be taken, not instrumentation, and the loop already
#          logs a rejected address two statements further down. Vanilla in
#          this same condition left the loop and went back to waiting on an
#          event that could no longer be armed, which is the silence
#          described above; there was no good behaviour here to preserve.
#
#          A standalone reproducer of the original loop - same constructs,
#          same MFC threading, built by the same compiler - is in wsrepro/
#          with its own workflow and its own README. It is what the words
#          above rest on: the original shape stops accepting after six to
#          194 connections in that environment and never once in eighteen
#          control runs on Windows, and the shape this patch installs ran
#          145,575 connections without a miss.
#   97     the code is wrong on every target, on input or on timing a peer
#          or a web client controls. 97a-c are the commits on the fork's
#          v0.72a-secfix branch, offered upstream as irwir/eMule#7; a file
#          here goes away when the same change reaches a released v0.72x.
#          The band held two more files, 97d and 97e, which changed how the
#          upload disk I/O thread is woken and where a finished send is
#          collected. Both are gone: measurement showed one of them waking
#          the thread hundreds of times a second for passes that prepared
#          nothing, and with it in place the other could not be measured at
#          all. What the measurement did find is 97f, which belongs to no
#          branch yet and corrects the two faults that actually end upload
#          slots. The allowance a socket gets before it is timed out is not
#          constant: an extension stops applying the moment the condition
#          that granted it stops being true, and the allowance then drops by
#          that whole extension between one call of the check and the next.
#          The time already spent was spent against the larger allowance, so
#          comparing it with the smaller one ends the connection in the same
#          instant, without it ever having had the smaller allowance to
#          spend; the patch starts the count again from the moment it
#          shrinks. And the extension an upload slot gets is granted by the
#          age of the slot, while the comment beside it says it is there for
#          a peer whose flow control is holding the send up. Those are not
#          the same set: it is given to slots that are not held up at all,
#          and withheld from the ones that are, so a socket with a full
#          queue and a send still in the hands of the operating system is
#          ended as though it had gone silent. The socket can answer that
#          question itself, so the patch asks it instead, bounded so that
#          the whole allowance stays what the age test already allowed.
#          97g corrects a second way an upload slot ends up doing nothing.
#          When a session is ended from this side - the transfer cap in
#          CheckForTimeOver() - the client is told to stand down and is put
#          back on the waiting list on its behalf, without it having asked
#          for anything. It is then the best candidate on that list almost
#          at once, because the waiting time a score is built from keeps
#          running through the whole upload session and is never reset, so
#          the client comes back holding more of it than anyone who really
#          waited. It is promoted again, on the same connection, one or two
#          seconds after being removed - and then sends no block request at
#          all, which is what the comment in the function doing the standing
#          down warns about. The slot stays empty until the socket times out.
#          Measured over three and a half hours: seven sessions ended this
#          way, seven promoted the same client straight back, six of those
#          delivered nothing and died at the timeout, together holding a slot
#          for twelve minutes to move 370 KB. The seventh freed itself only
#          because the client closed the connection on its own. The flag that
#          marks this state is already kept, from the moment the packet is
#          sent until the connection ends, and nothing reads it - here and in
#          0.60d alike. The patch gives it a reader, in each of the three
#          places that hand out a slot: the offer made from the waiting list,
#          the direct add when that list is empty, and the priority slot a
#          collection bypasses the queue with. Such a client is passed over
#          while it still has that connection, and asks again like everyone
#          else once it is gone. The fourth way in, the one kept for a lowID
#          client that missed its slot, needs a flag which is cleared when a
#          slot is given and which this client cannot get back while it is
#          being passed over, so it is closed by the first reader already.
#          97h corrects where a finished send is collected. Each upload
#          socket keeps one overlapped send outstanding, and its completion
#          is taken in a single place - CEMSocket::IsBusyExtensiveCheck() -
#          which the throttler reaches for the sockets inside the scan
#          window that still have something queued. The event those sends
#          carry is shared by every socket, so one wake-up stands for a
#          completion on any of them while those two conditions answer for
#          one. A send that finishes with its queue empty, or on a slot past
#          the end of the window, is then never collected: every sending
#          path uses the cheap busy check, which reads the flag that is
#          still raised and skips the socket, so nothing sends on it,
#          nothing queues more data behind it, and neither condition can
#          become true again. The flag that hides the completion is the flag
#          that keeps the collector away, and the slot does nothing for the
#          rest of its life. The patch hoists the bound of the scan into a
#          local and then, under the same lock, collects from the slots the
#          scan could not reach and only from those: the ones past that
#          bound, and the ones inside it whose queue is empty. A slot the
#          scan did reach has just been asked, and asking it again would
#          cost a lock and a system call; if its send completes in between,
#          the scan returns to it on the next pass, because both conditions
#          that took it there still hold. The slots collected here have no
#          such next pass, which is the whole reason for the loop. Both
#          tests are plain reads of memory this thread already holds the
#          queue lock for.
#
#          A first version asked every slot with a send outstanding. It was
#          run for eight hours on Wine 11 aarch64 and measured: 13,532,503
#          calls to WSAGetOverlappedResult, each taking the socket's send
#          lock, to collect 19 completions that nothing else would have
#          taken - 16 on a slot with an empty queue, 3 past the end of the
#          scan. Every one of the 19 satisfies the condition above, so the
#          narrower loop collects the same slots at a small fraction of the
#          cost. The longest of them had been outstanding 64,699 ms; the
#          slot went back to work and was still uploading fifteen minutes
#          later.
#
#          The shape is measured rather than proposed. ovrepro/ reproduces
#          the two gates around a real overlapped send, with its own
#          workflow and README. Eight slots and a window of three leave five
#          slots dead - slots 3 to 7, every one outside the window - on
#          Windows x64, on Windows Win32 and under Wine alike, which is what
#          places the fault in this arrangement rather than in anything
#          underneath it; the same harness sweeping the whole list leaves
#          none dead on any of the three. Wine completes no overlapped send
#          synchronously where Windows completes 96% of them that way, so
#          the condition arises there far more often - 1,277 episodes
#          against 12 - which decides how often it is met, not whether it is
#          there.
#
#          The band held one more file besides 97d and 97e above:
#          97i-robustness-iocp-failed-completion, which taught both
#          completion-port threads to tell a completion that failed apart
#          from no completion at all, so that neither the drain nor the
#          thread's own wait could end on one. It is gone for the opposite
#          reason to theirs: not because measurement found it costly, but
#          because measurement never found it at all. The counter kept for
#          the outcome read zero in all 42,100 samples taken over six days,
#          and a correction with no reading behind it is not carried here.
#          The finding survives the patch: iocprepro/ still reproduces the
#          outcome on demand - GetQueuedCompletionStatus returning FALSE
#          with a valid overlapped pointer and error 38 from a read past
#          end of file, on Windows x64, on Windows Win32 and under Wine
#          alike - with its own workflow and README, so the day a field
#          reading does turn up, the patch is a rewrite of nothing. 99h,
#          which watched it from inside, went with it.
#
#   98     corrections to the shipped language resources, from the fork's
#          v0.72a-lang branch - source data for the `languages` target
#          rather than code, so they change no binary.
#   99     reserved for temporary diagnostics - patches that make a build
#          report something rather than fix it, and that are deleted once
#          they have been read. Nothing in this band changes behaviour, and
#          nothing outside it writes a log line, so a corrective patch can
#          be reviewed, kept or sent upstream without carrying any of this.
#          Every line the band writes goes through QueueDebugLogLineEx,
#          which returns at once unless the verbose preference is set, so
#          the whole band can also be silenced at run time without a build.
#
#          99a-diag-upload-path used to stand on its own: it touched no file
#          any corrective patch touched, so it applied to an unmodified tree
#          and was how the behaviour of a build without those patches was
#          measured. It no longer does. 97h hoists the bound of the busy
#          scan into a local, and this patch reports that bound and reads it
#          in the same loop it rewrites; it was rebased onto 97h and now
#          needs it, the way 99b needs 97f and 99c needs 97g. What the
#          property existed for has been spent:
#          the last log written before the change is the before side, and it
#          reads orphans=43310 orphanage=34333ms, ovstuck=4877 ovmax=21114ms
#          ovtotal=54680ms over 10,365,293 probes.
#          It reports the waiting queue and the upload list every ten
#          seconds, the block requests peers send and how many were queued
#          or dropped as already served, the disk I/O thread - including,
#          per caller that wakes it, how often it asked, how often the ask
#          reached the completion port rather than being folded into a flag,
#          how many passes it caused and in how many of those a read was
#          actually started, which is the only number that says a wakeup was
#          worth posting - and the bandwidth throttler: what it was allowed
#          to spend against what it did spend, how many of its loops found
#          every socket holding data blocked, and how far down its slot list
#          the busy scan actually looks, which is not the whole list. It
#          also watches for the one state that would say a slot is starved
#          rather than slow - block requests waiting while nothing is queued
#          on its socket and nothing is in flight - and reports it the
#          moment it appears rather than at the next tick.
#
#          Counting the requests dropped as already served turned out not to
#          be enough. Roughly half of every range peers ask for is one we
#          have already handed to the disk - 3,986 of 8,024 in the first
#          session measured - and vanilla drops all of them without a word,
#          so the count on its own could not say which kind of repeat it
#          was. It now can. Each served block is stamped with the moment it
#          was handed over, in a list kept in lockstep with the served list
#          itself, and the drop reports which range it was, how long ago
#          that range was served and how far down the history it was found.
#          Note what served means here, because the whole reading depends on
#          it: the read has been issued, not finished and not sent.
#
#          The other half of the question is not about the range but about
#          the packet that carried it. OP_REQUESTPARTS has room for three
#          ranges and is sent at that size whatever it has to say, so a
#          repeat travelling beside a fresh range costs nothing on the wire
#          and is simply the downloader filling the slots it would otherwise
#          leave empty. A packet made of repeats alone is a different event:
#          that is a peer asking again on its own account, and the only
#          shape that would be ours to answer for. Each packet is therefore
#          classified once, at the signal the caller sends after its three
#          slots, and only that one shape gets a line - silence on it means
#          every repeat we dropped arrived beside a request we answered.
#
#          The counters this added are incremented through
#          InterlockedIncrement rather than ++, which on a volatile operand
#          is deprecated in C++23 and is where every C5214 in this build
#          comes from. The rest of the band still uses ++ and still warns;
#          this is not the patch that changes that, but there was no reason
#          to add to the count.
#
#          Finally it names every way a client can ask for a slot and be
#          turned away. Five of those paths simply return: three say nothing
#          at all, and two carry a message wrapped in DEBUG_ONLY, which a
#          release build compiles out. So in the builds anyone actually runs
#          a refused client and a client that never asked leave the same
#          trace, which is none, and a queue that stays empty cannot be told
#          from a queue that is turning people away.
#
#          99b-diag-socket-and-timeout describes the socket underneath those
#          slots and the decision that removes them, and it is the one file
#          in the whole directory that does not apply on its own: it needs
#          97f. That is not an accident of patching. Both express the same
#          decision in the same thirty lines of the timeout check, and the
#          quantity the log reports - how long this socket has been unable
#          to take more data - is the quantity 97f introduced to decide on.
#          Splitting them further would mean keeping two clocks for one
#          event, which is worth less than the independence it would buy.
#          It adds, to every line describing a socket, whether a send is
#          outstanding and for how long, what the completion port says about
#          it, how much is queued behind it, how long the socket has been
#          refusing sends without interruption and how long the longest such
#          spell has been - which is how a peer that has stopped reading its
#          own socket becomes visible from inside the application, since
#          nothing else in it can see that - and which of the six callers
#          last pushed the timeout forward, with a count for each. On the
#          timeout itself it reports when it fires, when a socket passes
#          three quarters of its allowance and then survives, when a slot is
#          past its base allowance and alive only because an extension is
#          carrying it, and whenever the correction in 97f applies - that
#          last is the only way to see from outside that a socket which
#          would have ended was given the smaller allowance to spend.
#
#          99c-diag-queue-score reports the one decision nothing in the
#          program shows: which waiting client is offered the next free
#          slot, and why it rather than the others. The order is set by a
#          score built on a clock that starts the first time a client is
#          scored and is never reset when it is given a slot, so the time a
#          client spends uploading keeps counting as time waited, and a
#          client that has just been served can outrank one that has only
#          ever waited. From outside the two are indistinguishable: the log
#          carries when a client enters and leaves the queue, which is a
#          different and younger clock than the one the score uses. This
#          patch prints, at each offer, the chosen client with its score and
#          the age of that clock, the same for the best few it beat, how
#          many candidates were scored, and how many were passed over as
#          standing down - the only outward sign that the correction in 97g
#          applied, which it needs and is therefore the second file here
#          that does not apply on its own. Only clients the loop really
#          scores are reported: reading the clock of a skipped one would
#          write the very quantity being observed.
#
#          Vanilla reports none of this, so a slot that dies holding data
#          looks the same whether the peer stopped asking, the request was
#          dropped, the send never completed, or the timer simply ran out
#          because nothing was left to reset it.
#
#          99e-diag-overlapped-collection measures the one state the per-slot
#          line can only catch by sampling: a send the operating system has
#          already finished on a socket that is still marked as making it.
#          Only one function in the program collects such a completion, and
#          the flag it clears is the same flag that keeps the socket from
#          being offered another send, so the state ends nowhere on its own.
#          The sweep added here asks every slot, once a millisecond at most,
#          and asks nothing else: not whether the slot is within the scan
#          window the collector uses, and not whether anything is queued on
#          it, because those two conditions are precisely what keeps the
#          collector away, and a measurement sharing them would be blind to
#          the same sockets. Nothing is collected - the call that would is
#          the correction this measures the need for, and a build carrying
#          this patch behaves like one without it. Each episode is reported
#          where it begins, carrying both conditions as they stood, and where
#          it ends, carrying how long it lasted and whether the socket had
#          anything queued at that moment: the collector reachable today
#          needs a queue, so an episode ending without one was ended by
#          something else. Counting episodes rather than samples is the point.
#          Periodic sampling weights a state by how long it lasts, so the same
#          reading cannot say whether a correction shortened the episodes or
#          merely pushed them below the sampling interval, which is the one
#          question a before and after comparison has to answer.
#
#          99f-diag-gui-timer-stall reports the instrument failing. Everything
#          periodic in eMule hangs off one Win32 timer callback, and the only
#          call that drains the queued debug log in normal operation is inside
#          it. That callback is a WM_TIMER, which the system produces only
#          when the thread has no other message waiting, and the sockets post
#          one message per event to a window on that same thread. Under steady
#          network pressure the timer can stop being delivered for hours while
#          every other thread keeps running: the program works, the log goes
#          quiet, and the backlog is finally written carrying the timestamps
#          of the moment it recovered rather than of when it happened. A window
#          like that cannot be read as a timeline at all, and nothing in the
#          log says which windows they are. The throttler thread keeps running
#          throughout, so it is the one place that can see it and say so. It
#          watches the timer's own heartbeat, reports the stall as it begins
#          and again when it ends with how long it lasted and how many ticks
#          were missed, and carries the backlog depth on its periodic line.
#          Every duration is measured on the thread that is still running, so
#          it survives being written out long afterwards - which is the whole
#          point, because the timestamp cannot.
#
#          99g-diag-overlapped-sweep measures the correction in 97h from the
#          inside, and needs it. The line carries how many sockets the sweep
#          carried into the extensive check at all, which is what it costs;
#          how many completions it took; how many of those nothing else in
#          the program would have come back to; and the age of the oldest.
#          Since the sweep only reaches slots the scan could not, a
#          collection is by construction one nothing else would have made,
#          so the last two counters must agree - they are kept apart on
#          purpose, because a divergence would say the condition is letting
#          through a slot the scan had already asked. A send old enough that
#          its slot sat idle while it waited also gets a line of its own, at
#          most one a second, carrying the slot, the bound of the scan, and
#          which side of it the slot was on: inside means the queue was
#          empty, outside means the scan stopped short of it, and the
#          condition admits no third case. 99e is left alone and stays the
#          instrument for the other half of the question: it counts episodes
#          rather than samples, so a before and after can say whether the
#          episodes were shortened or merely pushed below a sampling interval.
# ---------------------------------------------------------------------------
find_package(Git REQUIRED)

function(emule_apply_patches _submodule _patch_dir)
    file(GLOB _patches "${CMAKE_SOURCE_DIR}/${_patch_dir}/*.patch")
    list(SORT _patches)
    foreach(_patch IN LISTS _patches)
        get_filename_component(_patch_name "${_patch}" NAME)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply --ignore-whitespace --check --reverse "${_patch}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/${_submodule}"
            RESULT_VARIABLE _already_applied
            OUTPUT_QUIET ERROR_QUIET
        )
        if(_already_applied EQUAL 0)
            message(STATUS "${_submodule}: ${_patch_dir}/${_patch_name} already applied")
        else()
            message(STATUS "${_submodule}: applying ${_patch_dir}/${_patch_name}")
            execute_process(
                COMMAND ${GIT_EXECUTABLE} apply --ignore-whitespace "${_patch}"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/${_submodule}"
                RESULT_VARIABLE _apply_result
            )
            if(NOT _apply_result EQUAL 0)
                message(FATAL_ERROR "${_submodule}: failed to apply ${_patch_dir}/${_patch_name}")
            endif()
        endif()
    endforeach()
endfunction()

# Applied up front, before any add_subdirectory() below - emule_build's own
# CMakeLists.txt reads emule.vcxproj at configure time, so a patch must never
# land after the directory that consumes it has already been processed.
# An empty (or patch-less) *_build directory is simply a no-op here, so these
# calls stay valid whether or not that submodule currently needs anything.
# The first four are the C++ submodules whose headers eMule actually includes:
# eMule is the only target that compiles at C++23 (see emule_build/
# CMakeLists.txt), but cryptopp/id3lib/resizablelib headers get pulled into
# its translation units and are therefore parsed under that same standard.
# miniupnp is plain C and is otherwise built vanilla (see further down); it is
# listed here only so its one warning fix has somewhere to live.
emule_apply_patches(emule emule_build)
emule_apply_patches(cryptopp cryptopp_build)
emule_apply_patches(id3lib id3lib_build)
emule_apply_patches(resizablelib resizablelib_build)
emule_apply_patches(miniupnp miniupnpc_build)

# ---------------------------------------------------------------------------
# mbedtls - vanilla git submodule (mbedtls/, with nested tf-psa-crypto/ and
# framework/ submodules), pinned to v4.2.0. Never edited in place. eMule
# needs two customizations on top of it, both supplied from outside the
# submodule via extension points mbedTLS itself documents for this purpose,
# so there is nothing to re-apply after updating the submodule to a newer
# tag:
# - mbedtls_build/emule_mbedtls_user_config.h defines MBEDTLS_THREADING_ALT,
#   wired in below via MBEDTLS_USER_CONFIG_FILE.
# - threading_alt.h (MBEDTLS_THREADING_ALT's actual implementation) is not
#   duplicated here - see the include_directories() call below.
# ---------------------------------------------------------------------------
set(MBEDTLS_USER_CONFIG_FILE "${CMAKE_SOURCE_DIR}/mbedtls_build/emule_mbedtls_user_config.h"
    CACHE FILEPATH "" FORCE)
# mbedTLS 4.0 cross-checks the legacy config against the PSA crypto config
# and errors ("MBEDTLS_THREADING_ALT must be configured in psa/crypto_config.h")
# if a threading option is set on only one side - confirmed by an actual
# build attempt with only MBEDTLS_USER_CONFIG_FILE set above.
set(TF_PSA_CRYPTO_USER_CONFIG_FILE "${CMAKE_SOURCE_DIR}/mbedtls_build/emule_psa_crypto_user_config.h"
    CACHE FILEPATH "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "Don't build mbedtls's own example programs" FORCE)

# GEN_FILES defaults OFF on Windows (mbedtls's CMakeLists.txt assumes a
# release tarball there, which ships its generated sources pre-built). A git
# submodule checkout has none of them, so this must be forced ON - confirmed
# necessary by an actual configure attempt (failed on missing
# psa_crypto_driver_wrappers_no_static.c / error.c / ssl_debug_helpers_generated.c
# without it). Requires Python 3 on PATH.
set(GEN_FILES ON CACHE BOOL "Generate mbedtls's auto-generated sources (required for a submodule checkout)" FORCE)

# threading_alt.h (eMule's own mbedtls_platform_mutex_t/condition_variable_t
# implementation on top of Win32 CRITICAL_SECTION, consumed as an unqualified
# #include "threading_alt.h" by both mbedtls/tf-psa-crypto's threading.c and
# srchybrid/TLSthreading.h) is not vendored in this repo: the eMule devs
# already ship the exact same file inside the emule submodule itself, at
# emule/mbedtls/tf-psa-crypto/include/mbedtls/threading_alt.h - a leftover
# from the project's pre-CMake build setup. Pointing include_directories()
# straight at it avoids a second copy to keep in sync. Directory-scoped (not
# target_include_directories): threading.c, compiled deep inside
# tf-psa-crypto's own CMake targets, needs to see it too - include_directories()
# here is inherited by everything add_subdirectory()'d below, without having
# to chase mbedtls's internal target names across versions.
include_directories("${CMAKE_SOURCE_DIR}/emule/mbedtls/tf-psa-crypto/include/mbedtls")

# ---------------------------------------------------------------------------
# Crypto++ - vanilla git submodule (cryptopp/, weidai11/cryptopp pinned to
# CRYPTOPP_8_9_0), built via our own cryptopp_build/CMakeLists.txt since the
# upstream repo ships no CMake support of its own (see that file for why).
# ---------------------------------------------------------------------------
add_subdirectory(cryptopp_build)

# ---------------------------------------------------------------------------
# zlib - vanilla git submodule (zlib/, madler/zlib pinned to v1.3.2). Has its
# own CMakeLists.txt, so add_subdirectory() alone is enough; only the static
# lib is needed (matches every other dependency here - MFC linked static) and
# ZLIB_INSTALL would otherwise add install() rules we don't want for a
# vendored submodule. Built before id3lib_build: id3lib links against this
# zlib (zlibstatic target) instead of its own bundled zlib 1.1.4 copy - see
# id3lib_build/CMakeLists.txt.
# ---------------------------------------------------------------------------
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "eMule only needs the static zlib lib" FORCE)
set(ZLIB_BUILD_STATIC ON CACHE BOOL "eMule only needs the static zlib lib" FORCE)
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "Don't build zlib's own example programs" FORCE)
set(ZLIB_INSTALL OFF CACHE BOOL "Don't add install() rules for a vendored submodule" FORCE)
add_subdirectory(zlib)

# ---------------------------------------------------------------------------
# id3lib - vanilla git submodule (id3lib/, avwarez/id3lib pinned to v3.9.1),
# built via our own id3lib_build/CMakeLists.txt since the upstream repo ships
# no CMake support of its own (see that file for why).
# ---------------------------------------------------------------------------
add_subdirectory(id3lib_build)

# ---------------------------------------------------------------------------
# miniupnpc - near-vanilla git submodule (miniupnp/, miniupnp/miniupnp pinned
# to tag miniupnpc_2_3_3), carrying one warning patch from miniupnpc_build/.
# That's the upstream monorepo (also contains
# miniupnpd, minissdpd, ...) - only its miniupnpc/ subdirectory is built,
# which has its own CMakeLists.txt (add_subdirectory() alone is enough).
# Static lib only, no samples/tests/install, matching every other dependency
# here.
# ---------------------------------------------------------------------------
set(UPNPC_BUILD_STATIC ON CACHE BOOL "eMule only needs the static miniupnpc lib" FORCE)
set(UPNPC_BUILD_SHARED OFF CACHE BOOL "eMule only needs the static miniupnpc lib" FORCE)
set(UPNPC_BUILD_TESTS OFF CACHE BOOL "Don't build miniupnpc's own test executables" FORCE)
set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "Don't build miniupnpc's own sample executables" FORCE)
set(UPNPC_NO_INSTALL ON CACHE BOOL "Don't add install() rules for a vendored submodule" FORCE)
add_subdirectory(miniupnp/miniupnpc)

# ---------------------------------------------------------------------------
# ResizableLib - git submodule (resizablelib/, avwarez/resizablelib pinned to
# branch 20260917, which carries the High DPI work done on top of v1.5.3),
# built via our own resizablelib_build/CMakeLists.txt since the upstream repo
# ships no CMake support of its own (see that file for why). That file reads
# the source list out of ResizableLib.vcxproj rather than listing it, so the
# file this branch adds - ResizableDialogEx.cpp - is picked up on its own.
# ---------------------------------------------------------------------------
add_subdirectory(resizablelib_build)

add_subdirectory(mbedtls)

# ---------------------------------------------------------------------------
# eMule sources - vanilla git submodule (emule/, avwarez/eMule pinned to
# branch v0.72a), never edited in place. Everything it needs on top of that
# tag is supplied as patch files under emule_build/, applied by
# emule_apply_patches() near the top of this file.
# ---------------------------------------------------------------------------
add_subdirectory(emule_build)
