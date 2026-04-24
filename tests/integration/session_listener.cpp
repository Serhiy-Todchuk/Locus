#include "harness_fixture.h"

#include <catch2/catch_session.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

namespace {

// Catch2 listener that tears down the shared integration harness once all
// tests have finished running. Registered via CATCH_REGISTER_LISTENER so it
// is attached automatically by the default Catch2 main.
class IntegrationSessionListener : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunEnded(const Catch::TestRunStats& /*stats*/) override
    {
        locus::integration::IntegrationHarness::shutdown_shared();
    }
};

} // namespace

CATCH_REGISTER_LISTENER(IntegrationSessionListener)
