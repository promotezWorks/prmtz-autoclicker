// Automated test suite for promotezz AutoClicker in Goose

import std;
import math;

fn calculate_interval_ms(cps: f64) -> f64 {
    if cps <= 0.0 { return 100.0; }
    1000.0 / cps
}

fn test_cps_calculations() -> bool {
    let delay_10 = calculate_interval_ms(10.0);
    let delay_20 = calculate_interval_ms(20.0);
    let delay_14 = calculate_interval_ms(14.0);

    print("[TEST] 10 CPS delay: ", delay_10, " ms (expected 100.0)");
    print("[TEST] 20 CPS delay: ", delay_20, " ms (expected 50.0)");
    print("[TEST] 14 CPS delay: ", delay_14, " ms (expected ~71.43)");

    abs(delay_10 - 100.0) < 0.01 && abs(delay_20 - 50.0) < 0.01
}

fn test_gaussian_bounds() -> bool {
    // Verify random jitter properties
    var r = rng { 42 };
    var pass = true;
    for i in 100 {
        let val = r.rand_flt();
        if val < 0.0 || val > 1.0 { pass = false; }
    }
    print("[TEST] Random generator bounds: ", if pass { "PASSED" } else { "FAILED" });
    pass
}

fn main() {
    print("Running Goose AutoClicker Test Suite...");
    let ok1 = test_cps_calculations();
    let ok2 = test_gaussian_bounds();

    if ok1 && ok2 {
        print("[SUCCESS] All Goose AutoClicker tests passed!");
    } else {
        print("[ERROR] Tests failed.");
    }
}
