# -*- coding: utf-8 -*-
"""
Test metering and floating behaviors of DRM Library.
"""
from time import sleep
from random import randint
from datetime import datetime, timedelta
from re import search
import pytest


@pytest.mark.minimum
def test_metered_start_stop_short_time(accelize_drm, conf_json, cred_json, async_handler):
    """
    Test no error occurs in normal start/stop metering mode during a short period of time
    """
    driver = accelize_drm.pytest_fpga_driver[0]
    async_cb = async_handler.create()
    activators = accelize_drm.pytest_fpga_activators[0]
    activators.reset_coin()
    activators.autotest()
    cred_json.set_user('accelize_accelerator_test_02')

    async_cb.reset()
    conf_json.reset()
    drm_manager = accelize_drm.DrmManager(
        conf_json.path,
        cred_json.path,
        driver.read_register_callback,
        driver.write_register_callback,
        async_cb.callback
    )
    try:
        assert not drm_manager.get('license_status')
        activators.autotest(is_activated=False)
        activators[0].generate_coin(1000)
        drm_manager.activate()
        sleep(1)
        activators[0].check_coin(drm_manager.get('metered_data'))
        assert drm_manager.get('license_status')
        activators.autotest(is_activated=True)
        activators[0].generate_coin(10)
        activators[0].check_coin(drm_manager.get('metered_data'))
        drm_manager.deactivate()
        assert not drm_manager.get('license_status')
        activators.autotest(is_activated=False)
        coins = drm_manager.get('metered_data')
        assert coins == 0
        async_cb.assert_NoError()
    finally:
        drm_manager.deactivate()


def test_metered_start_stop_short_time_in_debug(accelize_drm, conf_json, cred_json, async_handler):
    """
    Test no error occurs in normal start/stop metering mode during a short period of time
    """
    driver = accelize_drm.pytest_fpga_driver[0]
    async_cb = async_handler.create()
    activators = accelize_drm.pytest_fpga_activators[0]
    activators.reset_coin()
    activators.autotest()
    cred_json.set_user('accelize_accelerator_test_02')

    async_cb.reset()
    conf_json.reset()
    conf_json['settings']['log_verbosity'] = 1
    conf_json.save()
    drm_manager = accelize_drm.DrmManager(
        conf_json.path,
        cred_json.path,
        driver.read_register_callback,
        driver.write_register_callback,
        async_cb.callback
    )
    try:
        assert not drm_manager.get('license_status')
        activators.autotest(is_activated=False)
        drm_manager.activate()
        sleep(1)
        assert drm_manager.get('metered_data') == 0
        assert drm_manager.get('license_status')
        activators.autotest(is_activated=True)
        activators[0].generate_coin(10)
        activators[0].check_coin(drm_manager.get('metered_data'))
        drm_manager.deactivate()
        assert not drm_manager.get('license_status')
        activators.autotest(is_activated=False)
        assert drm_manager.get('metered_data') == 0
        async_cb.assert_NoError()
    finally:
        drm_manager.deactivate()


@pytest.mark.long_run
def test_metered_start_stop_long_time(accelize_drm, conf_json, cred_json, async_handler):
    """
    Test no error occurs in normal start/stop metering mode during a long period of time
    """
    driver = accelize_drm.pytest_fpga_driver[0]
    async_cb = async_handler.create()
    activators = accelize_drm.pytest_fpga_activators[0]
    activators.reset_coin()
    activators.autotest()
    cred_json.set_user('accelize_accelerator_test_02')

    async_cb.reset()
    conf_json.reset()
    drm_manager = accelize_drm.DrmManager(
        conf_json.path,
        cred_json.path,
        driver.read_register_callback,
        driver.write_register_callback,
        async_cb.callback
    )
    try:
        assert not drm_manager.get('license_status')
        activators.autotest(is_activated=False)
        drm_manager.activate()
        start = datetime.now()
        license_duration = drm_manager.get('license_duration')
        assert drm_manager.get('license_status')
        assert drm_manager.get('metered_data') == 0
        activators.autotest(is_activated=True)
        activators[0].generate_coin(10)
        activators[0].check_coin(drm_manager.get('metered_data'))
        for i in range(3):
            wait_period = randint(license_duration-2, license_duration+2)
            sleep(wait_period)
            start += timedelta(seconds=license_duration)
            new_coins = randint(1,10)
            activators[0].generate_coin(new_coins)
            activators[0].check_coin(drm_manager.get('metered_data'))
        drm_manager.deactivate()
        assert not drm_manager.get('license_status')
        activators.autotest(is_activated=False)
        async_cb.assert_NoError()
    finally:
        drm_manager.deactivate()


@pytest.mark.minimum
def test_metered_pause_resume_short_time(accelize_drm, conf_json, cred_json, async_handler):
    """
    Test no error occurs in normal pause/resume metering mode during a short period of time
    """
    driver = accelize_drm.pytest_fpga_driver[0]
    async_cb = async_handler.create()
    activators = accelize_drm.pytest_fpga_activators[0]
    activators.reset_coin()
    activators.autotest()
    cred_json.set_user('accelize_accelerator_test_02')

    async_cb.reset()
    conf_json.reset()
    drm_manager = accelize_drm.DrmManager(
        conf_json.path,
        cred_json.path,
        driver.read_register_callback,
        driver.write_register_callback,
        async_cb.callback
    )
    try:
        assert not drm_manager.get('session_status')
        assert not drm_manager.get('license_status')
        activators.autotest(is_activated=False)
        drm_manager.activate()
        start = datetime.now()
        assert drm_manager.get('metered_data') == 0
        assert drm_manager.get('session_status')
        assert drm_manager.get('license_status')
        session_id = drm_manager.get('session_id')
        assert len(session_id) > 0
        activators.autotest(is_activated=True)
        lic_duration = drm_manager.get('license_duration')
        assert drm_manager.get('metered_data') == 0
        activators[0].generate_coin(10)
        activators[0].check_coin(drm_manager.get('metered_data'))
        drm_manager.deactivate(True)
        assert drm_manager.get('session_status')
        assert drm_manager.get('license_status')
        assert drm_manager.get('session_id') == session_id
        activators.autotest(is_activated=True)
        # Wait right before license expiration
        wait_period = start + timedelta(seconds=2*lic_duration-2) - datetime.now()
        sleep(wait_period.total_seconds())
        assert drm_manager.get('session_status')
        assert drm_manager.get('license_status')
        assert drm_manager.get('session_id') == session_id
        activators.autotest(is_activated=True)
        # Wait expiration
        sleep(4)
        assert drm_manager.get('session_status')
        assert drm_manager.get('session_id') == session_id
        assert not drm_manager.get('license_status')
        activators.autotest(is_activated=False)
        drm_manager.activate(True)
        assert drm_manager.get('session_status')
        assert drm_manager.get('session_id') == session_id
        assert drm_manager.get('license_status')
        activators.autotest(is_activated=True)
        drm_manager.deactivate()
        assert not drm_manager.get('session_status')
        assert not drm_manager.get('license_status')
        activators.autotest(is_activated=False)
        assert drm_manager.get('session_id') != session_id
        async_cb.assert_NoError()
    finally:
        drm_manager.deactivate()


@pytest.mark.long_run
def test_metered_pause_resume_long_time(accelize_drm, conf_json, cred_json, async_handler):
    """
    Test no error occurs in normal start/stop metering mode during a long period of time
    """
    driver = accelize_drm.pytest_fpga_driver[0]
    async_cb = async_handler.create()
    activators = accelize_drm.pytest_fpga_activators[0]
    activators.reset_coin()
    activators.autotest()
    cred_json.set_user('accelize_accelerator_test_02')

    async_cb.reset()
    conf_json.reset()
    drm_manager = accelize_drm.DrmManager(
        conf_json.path,
        cred_json.path,
        driver.read_register_callback,
        driver.write_register_callback,
        async_cb.callback
    )
    try:
        assert not drm_manager.get('session_status')
        assert not drm_manager.get('license_status')
        activators.autotest(is_activated=False)
        async_cb.assert_NoError()
        drm_manager.activate()
        start = datetime.now()
        assert drm_manager.get('metered_data') == 0
        assert drm_manager.get('session_status')
        assert drm_manager.get('license_status')
        session_id = drm_manager.get('session_id')
        assert len(session_id) > 0
        lic_duration = drm_manager.get('license_duration')
        activators.autotest(is_activated=True)
        coins = drm_manager.get('metered_data')
        for i in range(3):
            new_coins = randint(1, 100)
            activators[0].generate_coin(new_coins)
            activators[0].check_coin(drm_manager.get('metered_data'))
            drm_manager.deactivate(True)
            async_cb.assert_NoError()
            assert drm_manager.get('session_status')
            assert drm_manager.get('license_status')
            assert drm_manager.get('session_id') == session_id
            # Wait randomly
            nb_lic_expired = int((datetime.now() - start).total_seconds() / lic_duration)
            random_wait = randint((nb_lic_expired+2)*lic_duration-2, (nb_lic_expired+2)*lic_duration+2)
            wait_period = start + timedelta(seconds=random_wait) - datetime.now()
            sleep(wait_period.total_seconds())
            drm_manager.activate(True)
            start = datetime.now()
        assert drm_manager.get('session_status')
        assert drm_manager.get('session_id') == session_id
        assert drm_manager.get('license_status')
        activators.autotest(is_activated=True)
        drm_manager.deactivate()
        assert not drm_manager.get('session_status')
        assert not drm_manager.get('license_status')
        activators.autotest(is_activated=False)
        assert drm_manager.get('session_id') != session_id
        async_cb.assert_NoError()
    finally:
        drm_manager.deactivate()


@pytest.mark.minimum
@pytest.mark.no_parallel
def test_metering_limits(accelize_drm, conf_json, cred_json, async_handler, ws_admin):
    """
    Test an error is returned and the design is locked when the limit is reached.
    """
    driver = accelize_drm.pytest_fpga_driver[0]
    async_cb = async_handler.create()
    activators = accelize_drm.pytest_fpga_activators[0]
    activators.reset_coin()
    activators.autotest()
    cred_json.set_user('accelize_accelerator_test_03')

    # Test activate function call fails when limit is reached
    async_cb.reset()
    conf_json.reset()
    accelize_drm.clean_metering_env(cred_json, ws_admin)
    drm_manager = accelize_drm.DrmManager(
        conf_json.path,
        cred_json.path,
        driver.read_register_callback,
        driver.write_register_callback,
        async_cb.callback
    )
    try:
        assert drm_manager.get('license_type') == 'Floating/Metering'
        assert not drm_manager.get('license_status')
        drm_manager.activate()
        assert drm_manager.get('drm_license_type') == 'Floating/Metering'
        assert drm_manager.get('license_status')
        assert drm_manager.get('metered_data') == 0
        activators[0].generate_coin(999)
        activators[0].check_coin(drm_manager.get('metered_data'))
        sleep(1)
        drm_manager.deactivate()
        activators[0].reset_coin()
        assert not drm_manager.get('license_status')
        drm_manager.activate()
        assert drm_manager.get('license_status')
        activators[0].check_coin(drm_manager.get('metered_data'))
        activators[0].generate_coin(1)
        activators[0].check_coin(drm_manager.get('metered_data'))
        sleep(1)
        drm_manager.deactivate()
        assert not drm_manager.get('license_status')
        with pytest.raises(accelize_drm.exceptions.DRMWSReqError) as excinfo:
            drm_manager.activate()
        assert 'License Web Service error 400' in str(excinfo.value)
        assert 'DRM WS request failed' in str(excinfo.value)
        assert search(r'\\"Entitlement Limit Reached\\" with .+ for accelize_accelerator_test_03@accelize.com', str(excinfo.value))
        assert 'You have reached the maximum quantity of 1000. usage_unit for metered entitlement (licensed)' in str(excinfo.value)
        assert async_handler.get_error_code(str(excinfo.value)) == accelize_drm.exceptions.DRMWSReqError.error_code
        async_cb.assert_NoError()
    finally:
        drm_manager.deactivate()
    print('Test activate function fails when limit is reached: PASS')

    # Test background thread stops when limit is reached
    async_cb.reset()
    conf_json.reset()
    accelize_drm.clean_metering_env(cred_json, ws_admin)
    activators.reset_coin()
    drm_manager = accelize_drm.DrmManager(
        conf_json.path,
        cred_json.path,
        driver.read_register_callback,
        driver.write_register_callback,
        async_cb.callback
    )
    try:
        assert drm_manager.get('license_type') == 'Floating/Metering'
        assert not drm_manager.get('license_status')
        drm_manager.activate()
        start = datetime.now()
        assert drm_manager.get('drm_license_type') == 'Floating/Metering'
        assert drm_manager.get('license_status')
        assert drm_manager.get('metered_data') == 0
        lic_duration = drm_manager.get('license_duration')
        sleep(2)
        activators[0].generate_coin(1000)
        activators[0].check_coin(drm_manager.get('metered_data'))
        # Wait right before expiration
        wait_period = start + timedelta(seconds=3*lic_duration-3) - datetime.now()
        sleep(wait_period.total_seconds())
        assert drm_manager.get('license_status')
        activators.autotest(is_activated=True)
        sleep(5)
        assert not drm_manager.get('license_status')
        activators.autotest(is_activated=False)
        # Verify asynchronous callback has been called
        assert async_cb.was_called
        assert 'License Web Service error 400' in async_cb.message
        assert 'DRM WS request failed' in async_cb.message
        assert search(r'\\"Entitlement Limit Reached\\" with .+ for accelize_accelerator_test_03@accelize.com', async_cb.message)
        assert 'You have reached the maximum quantity of 1000. usage_unit for metered entitlement (licensed)' in async_cb.message
        assert async_cb.errcode == accelize_drm.exceptions.DRMWSReqError.error_code
        drm_manager.deactivate()
        assert not drm_manager.get('license_status')
        activators.autotest(is_activated=False)
    finally:
        drm_manager.deactivate()
    print('Test background thread stops when limit is reached: PASS')


@pytest.mark.on_2_fpga
@pytest.mark.minimum
def test_floating_limits(accelize_drm, conf_json, cred_json, async_handler):
    """
    Test an error is returned when the floating limit is reached
    """
    driver0 = accelize_drm.pytest_fpga_driver[0]
    driver1 = accelize_drm.pytest_fpga_driver[1]
    async_cb0 = async_handler.create()
    async_cb1 = async_handler.create()

    cred_json.set_user('accelize_accelerator_test_04')
    conf_json.reset()

    async_cb0.reset()
    drm_manager0 = accelize_drm.DrmManager(
        conf_json.path,
        cred_json.path,
        driver0.read_register_callback,
        driver0.write_register_callback,
        async_cb0.callback
    )
    async_cb1.reset()
    drm_manager1 = accelize_drm.DrmManager(
        conf_json.path,
        cred_json.path,
        driver1.read_register_callback,
        driver1.write_register_callback,
        async_cb1.callback
    )
    assert not drm_manager0.get('license_status')
    assert not drm_manager1.get('license_status')
    try:
        drm_manager0.activate()
        assert drm_manager0.get('license_status')
        with pytest.raises(accelize_drm.exceptions.DRMWSError) as excinfo:
            drm_manager1.activate()
        assert search(r'Timeout on License request after .+ attempts', str(excinfo.value)) is not None
        assert async_handler.get_error_code(str(excinfo.value)) == accelize_drm.exceptions.DRMWSError.error_code
        async_cb1.assert_NoError()
    finally:
        drm_manager0.deactivate()
        assert not drm_manager0.get('license_status')
        async_cb0.assert_NoError()
    try:
        drm_manager1.activate()
        assert drm_manager1.get('license_status')
        with pytest.raises(accelize_drm.exceptions.DRMWSError) as excinfo:
            drm_manager0.activate()
        assert search(r'Timeout on License request after .+ attempts', str(excinfo.value)) is not None
        assert async_handler.get_error_code(str(excinfo.value)) == accelize_drm.exceptions.DRMWSError.error_code
        async_cb0.assert_NoError()
    finally:
        drm_manager1.deactivate()
        assert not drm_manager1.get('license_status')
        async_cb1.assert_NoError()
