// Adapted from Xbox2Windows tests/intake/forwarders_check.py at
// 939e5142458007405bbc7a2a6d14bad292fcae62. MIT; see LICENSE.Xbox2Windows.
// These declarative cases are tested through native PE execution here.
struct ForwarderCase {
    const char *name;
    const char *target;
    bool resolves;
};

constexpr ForwarderCase forwarder_cases[] = {
    {"forwarded_value", "intake_provider.named_value", true},
    {"forwarded_ordinal", "intake_provider.#77", true},
    {"forwarded_chain", "intake_forward.forwarded_value", true},
    {"forwarded_missing", "intake_provider.absent_value", false},
    {"forwarded_loop", "intake_forward.forwarded_loop", false},
    {"forwarded_absent", "intake_absent.absent_value", false},
    {"forwarded_upper", "INTAKE_PROVIDER.named_value", true},
};
