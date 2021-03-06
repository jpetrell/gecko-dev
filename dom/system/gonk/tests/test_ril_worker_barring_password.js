/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

subscriptLoader.loadSubScript("resource://gre/modules/ril_consts.js", this);

function run_test() {
  run_next_test();
}

const PIN = "0000";
const NEW_PIN = "1234";

add_test(function test_change_call_barring_password() {
  let worker = newUint8Worker();
  let context = worker.ContextPool._contexts[0];
  let buf = context.Buf;

  function do_test(facility, pin, newPin) {
    buf.sendParcel = function fakeSendParcel () {
      // Request Type.
      equal(this.readInt32(), REQUEST_CHANGE_BARRING_PASSWORD);

      // Token : we don't care.
      this.readInt32();

      let parcel = this.readStringList();
      equal(parcel.length, 3);
      equal(parcel[0], facility);
      equal(parcel[1], pin);
      equal(parcel[2], newPin);
    };

    let options = {facility: facility, pin: pin, newPin: newPin};
    context.RIL.changeCallBarringPassword(options);
  }

  do_test(ICC_CB_FACILITY_BA_ALL, PIN, NEW_PIN);

  run_next_test();
});

add_test(function test_check_change_call_barring_password_result() {
  let barringPasswordOptions;
  let workerHelper = newInterceptWorker();
  let worker = workerHelper.worker;

  let context = worker.ContextPool._contexts[0];
  context.RIL.changeCallBarringPassword =
    function fakeChangeCallBarringPassword(options) {
      barringPasswordOptions = options;
      context.RIL[REQUEST_CHANGE_BARRING_PASSWORD](0, {
        rilRequestError: ERROR_SUCCESS
      });
    };

  context.RIL.changeCallBarringPassword({pin: PIN, newPin: NEW_PIN});

  let postedMessage = workerHelper.postedMessage;
  equal(barringPasswordOptions.pin, PIN);
  equal(barringPasswordOptions.newPin, NEW_PIN);

  run_next_test();
});
