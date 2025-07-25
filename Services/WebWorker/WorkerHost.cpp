/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/File.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibWeb/Fetch/Enums.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/HTML/DedicatedWorkerGlobalScope.h>
#include <LibWeb/HTML/MessageEvent.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/EnvironmentSettingsSnapshot.h>
#include <LibWeb/HTML/Scripting/Fetching.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Scripting/WorkerEnvironmentSettingsObject.h>
#include <LibWeb/HTML/SharedWorkerGlobalScope.h>
#include <LibWeb/HTML/WorkerDebugConsoleClient.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <WebWorker/WorkerHost.h>

namespace WebWorker {

WorkerHost::WorkerHost(URL::URL url, Web::Bindings::WorkerType type, String name)
    : m_url(move(url))
    , m_type(type)
    , m_name(move(name))
{
}

WorkerHost::~WorkerHost() = default;

// https://html.spec.whatwg.org/multipage/workers.html#run-a-worker
void WorkerHost::run(GC::Ref<Web::Page> page, Web::HTML::TransferDataEncoder message_port_data, Web::HTML::SerializedEnvironmentSettingsObject const& outside_settings_snapshot, Web::Bindings::RequestCredentials credentials, bool is_shared)
{
    // 3. Let unsafeWorkerCreationTime be the unsafe shared current time.
    auto unsafe_worker_creation_time = Web::HighResolutionTime::unsafe_shared_current_time();

    // 7. Let realm execution context be the result of creating a new JavaScript realm given agent and the following customizations:
    auto realm_execution_context = Web::Bindings::create_a_new_javascript_realm(
        Web::Bindings::main_thread_vm(),
        [page, is_shared](JS::Realm& realm) -> JS::Object* {
            //      7a. For the global object, if is shared is true, create a new SharedWorkerGlobalScope object.
            //      7b. Otherwise, create a new DedicatedWorkerGlobalScope object.
            if (is_shared)
                return Web::Bindings::main_thread_vm().heap().allocate<Web::HTML::SharedWorkerGlobalScope>(realm, page);
            return Web::Bindings::main_thread_vm().heap().allocate<Web::HTML::DedicatedWorkerGlobalScope>(realm, page);
        },
        nullptr);

    // 8. Let worker global scope be the global object of realm execution context's Realm component.
    // NOTE: This is the DedicatedWorkerGlobalScope or SharedWorkerGlobalScope object created in the previous step.
    GC::Ref<Web::HTML::WorkerGlobalScope> worker_global_scope = as<Web::HTML::WorkerGlobalScope>(realm_execution_context->realm->global_object());

    // 9. Set up a worker environment settings object with realm execution context,
    //    outside settings, and unsafeWorkerCreationTime, and let inside settings be the result.
    auto inside_settings = Web::HTML::WorkerEnvironmentSettingsObject::setup(page, move(realm_execution_context), outside_settings_snapshot, unsafe_worker_creation_time);

    auto& console_object = *inside_settings->realm().intrinsics().console_object();
    m_console = console_object.heap().allocate<Web::HTML::WorkerDebugConsoleClient>(console_object.console());
    VERIFY(m_console);
    console_object.console().set_client(*m_console);

    // 10. Set worker global scope's name to the value of options's name member.
    worker_global_scope->set_name(m_name);

    // 11. Append owner to worker global scope's owner set.
    // FIXME: support for 'owner' set on WorkerGlobalScope

    // IMPLEMENTATION DEFINED: We need an object to represent the fetch response's client
    auto outside_settings = inside_settings->realm().create<Web::HTML::EnvironmentSettingsSnapshot>(inside_settings->realm(), inside_settings->realm_execution_context().copy(), outside_settings_snapshot);

    // 12. If is shared is true, then:
    if (is_shared) {
        auto& shared_global_scope = static_cast<Web::HTML::SharedWorkerGlobalScope&>(*worker_global_scope);
        // 1. Set worker global scope's constructor origin to outside settings's origin.
        shared_global_scope.set_constructor_origin(outside_settings->origin());

        // 2. Set worker global scope's constructor url to url.
        shared_global_scope.set_constructor_url(m_url);

        // 3. Set worker global scope's type to the value of options's type member.
        shared_global_scope.set_type(m_type);

        // 4. Set worker global scope's credentials to the value of options's credentials member.
        shared_global_scope.set_credentials(Web::Fetch::from_bindings_enum(credentials));
    }

    // 13. Let destination be "sharedworker" if is shared is true, and "worker" otherwise.
    auto destination = is_shared ? Web::Fetch::Infrastructure::Request::Destination::SharedWorker
                                 : Web::Fetch::Infrastructure::Request::Destination::Worker;

    // In both cases, let performFetch be the following perform the fetch hook given request, isTopLevel, and processCustomFetchResponse:
    auto perform_fetch_function = [inside_settings, worker_global_scope, is_shared](GC::Ref<Web::Fetch::Infrastructure::Request> request, Web::HTML::TopLevelModule is_top_level, Web::Fetch::Infrastructure::FetchAlgorithms::ProcessResponseConsumeBodyFunction process_custom_fetch_response) -> Web::WebIDL::ExceptionOr<void> {
        auto& realm = inside_settings->realm();
        auto& vm = realm.vm();

        Web::Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};

        // 1. If isTopLevel is false, fetch request with processResponseConsumeBody set to processCustomFetchResponse, and abort these steps.
        if (is_top_level == Web::HTML::TopLevelModule::No) {
            fetch_algorithms_input.process_response_consume_body = move(process_custom_fetch_response);
            TRY(Web::Fetch::Fetching::fetch(realm, request, Web::Fetch::Infrastructure::FetchAlgorithms::create(vm, move(fetch_algorithms_input))));
            return {};
        }

        // 2. Set request's reserved client to inside settings.
        request->set_reserved_client(GC::Ptr<Web::HTML::EnvironmentSettingsObject>(inside_settings));

        // We need to store the process custom fetch response function on the heap here, because we're storing it in another heap function
        auto process_custom_fetch_response_function = GC::create_function(vm.heap(), move(process_custom_fetch_response));

        // 3. Fetch request with processResponseConsumeBody set to the following steps given response response and null, failure, or a byte sequence bodyBytes:
        fetch_algorithms_input.process_response_consume_body = [worker_global_scope, process_custom_fetch_response_function, inside_settings, is_shared](auto response, auto body_bytes) {
            auto& vm = inside_settings->vm();

            // 1. Set worker global scope's url to response's url.
            worker_global_scope->set_url(response->url().value_or({}));

            // 2. Initialize worker global scope's policy container given worker global scope, response, and inside settings.
            worker_global_scope->initialize_policy_container(response, inside_settings);

            // 3. If the Run CSP initialization for a global object algorithm returns "Blocked" when executed upon worker
            //    global scope, set response to a network error. [CSP]
            if (worker_global_scope->run_csp_initialization() == Web::ContentSecurityPolicy::Directives::Directive::Result::Blocked) {
                response = Web::Fetch::Infrastructure::Response::network_error(vm, "Blocked by Content Security Policy"_string);
            }

            // FIXME: Use worker global scope's policy container's embedder policy
            // FIXME: 4. If worker global scope's embedder policy's value is compatible with cross-origin isolation and is shared is true,
            //    then set agent's agent cluster's cross-origin isolation mode to "logical" or "concrete".
            //    The one chosen is implementation-defined.
            // FIXME: 5. If the result of checking a global object's embedder policy with worker global scope, outside settings,
            //    and response is false, then set response to a network error.
            // FIXME: 6. Set worker global scope's cross-origin isolated capability to true if agent's agent cluster's cross-origin
            //    isolation mode is "concrete".

            if (!is_shared) {
                // 7.  If is shared is false and owner's cross-origin isolated capability is false, then set worker
                //     global scope's cross-origin isolated capability to false.
                // 8. If is shared is false and response's url's scheme is "data", then set worker global scope's
                //     cross-origin isolated capability to false.
            }

            // 9. Run processCustomFetchResponse with response and bodyBytes.
            process_custom_fetch_response_function->function()(response, body_bytes);
        };
        TRY(Web::Fetch::Fetching::fetch(realm, request, Web::Fetch::Infrastructure::FetchAlgorithms::create(vm, move(fetch_algorithms_input))));
        return {};
    };
    auto perform_fetch = Web::HTML::create_perform_the_fetch_hook(inside_settings->heap(), move(perform_fetch_function));

    auto on_complete_function = [inside_settings, worker_global_scope, message_port_data = move(message_port_data), url = m_url, is_shared](GC::Ptr<Web::HTML::Script> script) mutable {
        auto& realm = inside_settings->realm();

        // 1. If script is null or if script's error to rethrow is non-null, then:
        if (!script || !script->error_to_rethrow().is_null()) {
            // FIXME: 1. Queue a global task on the DOM manipulation task source given worker's relevant global object to fire an event named error at worker.
            // FIXME: Notify Worker parent through IPC to fire an error event at Worker

            // 2. Run the environment discarding steps for inside settings.
            inside_settings->discard_environment();

            // 3. Abort these steps.
            dbgln("DedicatedWorkerHost: Unable to fetch script {} because {}", url, script ? script->error_to_rethrow().to_string_without_side_effects() : "script was null"_string);
            return;
        }

        // FIXME: 2. Associate worker with worker global scope.
        // What does this even mean?

        // 3. Let inside port be a new MessagePort object in inside settings's Realm.
        auto inside_port = Web::HTML::MessagePort::create(realm);

        // 4. Associate inside port with worker global scope.
        worker_global_scope->set_internal_port(inside_port);

        // 5. Entangle outside port and inside port.
        Web::HTML::TransferDataDecoder decoder { move(message_port_data) };
        MUST(inside_port->transfer_receiving_steps(decoder));

        // 6. Create a new WorkerLocation object and associate it with worker global scope.
        worker_global_scope->set_location(realm.create<Web::HTML::WorkerLocation>(*worker_global_scope));

        // FIXME: 7. Closing orphan workers: Start monitoring the worker such that no sooner than it
        //     stops being a protected worker, and no later than it stops being a permissible worker,
        //     worker global scope's closing flag is set to true.

        // FIXME: 8. Suspending workers: Start monitoring the worker, such that whenever worker global scope's
        //     closing flag is false and the worker is a suspendable worker, the user agent suspends
        //     execution of script in that worker until such time as either the closing flag switches to
        //     true or the worker stops being a suspendable worker

        // 9. Set inside settings's execution ready flag.
        inside_settings->execution_ready = true;

        // 10. If script is a classic script, then run the classic script script.
        //     Otherwise, it is a module script; run the module script script.
        if (is<Web::HTML::ClassicScript>(*script))
            (void)static_cast<Web::HTML::ClassicScript&>(*script).run();
        else
            (void)as<Web::HTML::JavaScriptModuleScript>(*script).run();

        // FIXME: 11. Enable outside port's port message queue.

        // 12. If is shared is false, enable the port message queue of the worker's implicit port.
        if (!is_shared) {
            inside_port->enable();
        }

        // 13. If is shared is true, then queue a global task on the DOM manipulation task source given worker
        //     global scope to fire an event named connect at worker global scope, using MessageEvent,
        //     with the data attribute initialized to the empty string, the ports attribute initialized
        //     to a new frozen array containing inside port, and the source attribute initialized to inside port.
        if (is_shared) {
            Web::HTML::queue_global_task(Web::HTML::Task::Source::DOMManipulation, *worker_global_scope, GC::create_function(realm.heap(), [worker_global_scope, inside_port] {
                auto& realm = worker_global_scope->realm();
                auto& vm = realm.vm();
                Web::HTML::TemporaryExecutionContext const context(realm);

                Web::HTML::MessageEventInit event_init {};
                event_init.data = GC::Ref { vm.empty_string() };
                event_init.ports = { inside_port };
                event_init.source = inside_port;

                auto message_event = Web::HTML::MessageEvent::create(realm, Web::HTML::EventNames::connect, event_init);
                worker_global_scope->dispatch_event(message_event);
            }));
        }

        // FIXME: 14. Enable the client message queue of the ServiceWorkerContainer object whose associated service
        //     worker client is worker global scope's relevant settings object.

        // 15. Event loop: Run the responsible event loop specified by inside settings until it is destroyed.
        inside_settings->responsible_event_loop().schedule();

        // FIXME: We need to react to the closing flag being set on the responsible event loop
        //        And use that to shutdown the WorkerHost
        // FIXME: 16. Clear the worker global scope's map of active timers.
        // FIXME: 17. Disentangle all the ports in the list of the worker's ports.
        // FIXME: 18. Empty worker global scope's owner set.
    };
    auto on_complete = Web::HTML::create_on_fetch_script_complete(inside_settings->vm().heap(), move(on_complete_function));

    // AD-HOC: Fetching a script performs actions such as for blobs checking that they are on the same partition
    //         based on origin. However, this is performed before the consume body algorithm is run, where
    //         this URL for that worker is set. As a workaround, set the URL upfront.
    worker_global_scope->set_url(m_url);

    // 14. Obtain script by switching on the value of options's type member:
    // classic:  Fetch a classic worker script given url, outside settings, destination, inside settings,
    //               and with onComplete and performFetch as defined below.
    // module:   Fetch a module worker script graph given url, outside settings, destination, the value of the credentials member of options, inside settings,
    //               and with onComplete and performFetch as defined below.
    if (m_type == Web::Bindings::WorkerType::Classic) {
        if (auto err = Web::HTML::fetch_classic_worker_script(m_url, outside_settings, destination, inside_settings, perform_fetch, on_complete); err.is_error()) {
            dbgln("Failed to run worker script");
            // FIXME: Abort the worker properly
            TODO();
        }
    } else {
        VERIFY(m_type == Web::Bindings::WorkerType::Module);
        // FIXME: Pass credentials
        if (auto err = Web::HTML::fetch_module_worker_script_graph(m_url, outside_settings, destination, inside_settings, perform_fetch, on_complete); err.is_error()) {
            dbgln("Failed to run worker script");
            // FIXME: Abort the worker properly
            TODO();
        }
    }
}

}
