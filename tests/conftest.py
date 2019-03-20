# -*- coding: utf-8 -*-
"""Configure Pytest"""
from os import environ, listdir, remove
from os.path import realpath, abspath, isfile, isdir, expanduser, splitext, join, dirname
from json import dump, load
from copy import deepcopy
from re import search
from ctypes import c_uint32, byref

import pytest

import sys
myPath = dirname(abspath(__file__))
sys.path.insert(0, myPath)
from ws_admin_functions import WSListFunction


_TESTS_PATH = dirname(realpath(__file__))
_SESSION = dict()
_LICENSING_SERVERS = dict(
    dev='https://master.devmetering.accelize.com',
    prod='https://master.metering.accelize.com')


def get_default_conf_json(licensing_server_url):
    """
    Get default "conf.json" file content as python dict.

    Args:
        licensing_server_url (str): Licensing server URL.

    Returns:
        dict: "conf.json" content
    """
    if licensing_server_url in _LICENSING_SERVERS.keys():
        url = _LICENSING_SERVERS.get(
            licensing_server_url.lower(), licensing_server_url)
    else:
        url = licensing_server_url

    return {
        "licensing": {
            "url": url,
        },
        "drm": {
            "frequency_mhz": 125
        },

        # 1.X.X API compatibility
        # TODO: Remove once expired
        "design": {
            "udid": "6AE1A700-0000-0000-0000-000000000001",
            "boardType": "DRM_125"
        },
        "webservice": {
            "oauth2_url": "%s/o/token/" % url,
            "metering_url":
                "%s/auth/metering/genlicense/" % url
        }
    }


# Pytest configuration

def pytest_addoption(parser):
    """
    Add command lines arguments
    """
    parser.addoption(
        "--backend", action="store", default="c++",
        help='Use specified Accelize DRM library API as backend: "c" or "c++"')
    parser.addoption(
        "--fpga_driver", action="store", default="aws_f1",
        help='Specify FPGA driver to use with DRM library')
    parser.addoption(
        "--fpga_slot_id", action="store", default=0, type=int,
        help='Specify the FPGA slot to use')
    parser.addoption(
        "--drm_controller_base_address", action="store", default=0, type=int,
        help='Specify the DRM controller base address')
    parser.addoption(
        "--cred", action="store", default="./cred.json",
        help='Specify cred.json path')
    parser.addoption(
        "--server", action="store",
        default="prod", help='Specify the metering server to use')
    parser.addoption(
        "--library_verbosity", action="store", default='2',
        help='Specify "libaccelize_drm" verbosity level')
    parser.addoption(
        "--library_logformat", action="store", default='0',
        help='Specify "libaccelize_drm" log format')
    parser.addoption(
        "--fpga_image", default="default",
        help='Select FPGA image to program the FPGA with. '
             'By default, use default FPGA image for the selected driver and '
             'last HDK version.')
    parser.addoption(
        "--hdk_version",
        help='Select FPGA image base on Accelize DRM HDK version. By default, '
             'use default FPGA image for the selected driver and last HDK '
             'version.')
    parser.addoption(
        "--integration", action="store_true",
        help='Run integration tests. Theses tests may needs two FPGAs.'
    )
    parser.addoption(
        "--activator_base_address", action="store", default=0x10000, type=int,
        help='Specify the lowest activator base address in the design')
    parser.addoption(
        "--activator_range_address", action="store", default=0x10000, type=int,
        help='Specify the lowest activator base address in the design')



def pytest_runtest_setup(item):
    """
    Configure test initialization
    """
    markers = tuple(item.iter_markers(name='integration'))
    if not item.config.getoption("integration") and markers:
        pytest.skip("Don't run integration tests.")
    elif item.config.getoption("integration") and not markers:
        pytest.skip("Run only integration tests.")


class ActivatorsInFPGA:
    """
    Activators object
    """
    def __init__(self, driver, base_address, address_range):
        self.driver = driver
        self.base_address = base_address
        self.address_range = address_range

    def generate_coin(self, coins, activator_index=0):
        value = c_uint32()
        for i in range(coins):
            self.driver.read_register_callback( self.base_address +
                           activator_index * self.address_range, byref(value) )

    def get_status(self, activator_index=None):
        if isinstance(activator_index, list):
            activator_index_list = activator_index
        elif isinstance(activator_index, int):
            activator_index_list = [activator_index]
        else:
            raise TypeError('Unsupported type: %s' % type(activator_index))
        regvalue = c_uint32(0)
        status_list = []
        for i in activator_index_list:
            self.driver.read_register_callback( self.base_address +
                           i * self.address_range, byref(regvalue) )
            value = regvalue.value
            code_rdy = (value >> 1) & 1
            active = value & 1
            status_list.append(active)
        if len(status_list) == 1 and activator_index is None or isinstance(activator_index, int):
            return status_list[0]
        return status_list


class RefDesign:
    """
    Handle HDK versions and their related FPGA image ID
    """
    def __init__(self, path):
        if not isdir(path):
            raise IOError("Following path must be a valid directory: %s" % path)
        self._path = path
        self.hdk_versions = sorted([splitext(file_name)[0].strip('v')
                               for file_name in listdir(self._path)
                               if file_name.endswith('.json')])

    def get_image_id(self, hdk_version=None):
        if hdk_version is None:
            hdk_version = self.hdk_versions[-1]
        with open(join(self._path, 'v%s.json' % hdk_version)) as hdk_json_file:
            hdk_json = load(hdk_json_file)
        for key in ('fpga_image', 'FpgaImageGlobalId', 'FpgaImageId'):
            try:
                return hdk_json[key]
            except KeyError:
                continue
        else:
            raise ValueError('No FPGA image found for %s.' % hdk_version)


# Pytest Fixtures

@pytest.fixture(scope='session')
def accelize_drm(pytestconfig):
    """
    Get Python Accelize DRM configured the proper way.
    """
    # Define if currently in build environment
    if isfile('CMakeCache.txt'):
        build_environment = True
        with open('CMakeCache.txt', 'rt') as cmake_cache:
            build_type = (
                'debug' if "CMAKE_BUILD_TYPE:STRING=Debug" in cmake_cache.read()
                else 'release')

        # Add Build environment to Python import hook
        # NOTE: It is not possible to setup "LD_LIBRARY_PATH" once Python is run
        #       this value need to be set prior to run "pytest" to allow
        #       python importing shared libraries.
        import sys
        sys.path.append(realpath('python3_bdist'))

    else:
        build_environment = False
        build_type = 'release'

    # Check cred.json
    cred_path = realpath(expanduser(pytestconfig.getoption("cred")))
    if not isfile(cred_path):
        raise ValueError('Credential file specified by "--cred" does not exist')

    # Select C or C++ based on environment and import Python Accelize Library
    backend = pytestconfig.getoption("backend")
    if backend == 'c':
        environ['ACCELIZE_DRM_PYTHON_USE_C'] = '1'

    elif backend != 'c++':
        raise ValueError('Invalid value for "--backend"')

    import accelize_drm as _accelize_drm

    # Get FPGA driver
    from python_fpga_drivers import get_driver
    fpga_driver_name = pytestconfig.getoption("fpga_driver")
    fpga_driver_cls = get_driver(fpga_driver_name)

    # Get FPGA image
    fpga_image = pytestconfig.getoption("fpga_image")
    hdk_version = pytestconfig.getoption("hdk_version")

    ref_designs = RefDesign(join(_TESTS_PATH, 'refdesigns', fpga_driver_name))

    if hdk_version and fpga_image.lower() != 'default':
        raise ValueError(
            'Please set "hdk_version" or "fpga_image" but not both')

    elif fpga_image.lower() == 'default' or hdk_version:
        # Use specified HDK version
        if hdk_version:
            hdk_version = hdk_version.strip('v')
            if hdk_version not in ref_designs.hdk_versions:
                raise ValueError((
                    'HDK version %s is not supported. '
                    'Available versions are: %s') % (
                    hdk_version, ", ".join(ref_designs.hdk_versions)))
        # Get last HDK version as default
        else:
            hdk_version = ref_designs.hdk_versions[-1]

        # Get FPGA image from HDK version
        fpga_image = ref_designs.get_image_id(hdk_version)

    # Define or get FPGA Slot
    if pytestconfig.getoption('integration'):
        # Integration tests requires 2 slots
        fpga_slot_id = [0, 1]
    elif environ.get('TOX_PARALLEL_ENV'):
        # Define FPGA slot for Tox parallel execution
        fpga_slot_id = [0 if backend == 'c' else 1]
    else:
        # Use user defined slot
        fpga_slot_id = [pytestconfig.getoption("fpga_slot_id")]

    # Initialize FPGA
    print('FPGA SLOT ID:', fpga_slot_id)
    print('FPGA IMAGE:', fpga_image)
    print('HDK VERSION:', hdk_version)
    fpga_driver = [fpga_driver_cls(
        fpga_slot_id=slot_id,
        fpga_image=fpga_image,
        drm_ctrl_base_addr=pytestconfig.getoption(
            "drm_controller_base_address"))
        for slot_id in fpga_slot_id]

    # Define Activator access
    fpga_activators = [ ActivatorsInFPGA(fpga_driver[i],
        pytestconfig.getoption("activator_base_address"),
        pytestconfig.getoption("activator_range_address"))
        for i in range(len(fpga_driver))]

    def cleanNodelockEnv(drm_manager, driver, conf_json, cred_json, ws_admin):
        # Clean license directory for nodelock
        conf_json.cleanNodelockDir()
        # Clear nodelock request from WS DB (not to hit the limit)
        product_info = {'library': 'refdesign', 'name': 'drm_1activator'}
        ws_admin.remove_product_information(product_info, cred_json['email'])
        # Reprogram FPGA
        if drm_manager.get('drm_license_type') == 'Node-Locked':
            driver.program_fpga()

    # Store some values for access in tests
    _accelize_drm.pytest_build_environment = build_environment
    _accelize_drm.pytest_build_source_dir = '@CMAKE_CURRENT_SOURCE_DIR@'
    _accelize_drm.pytest_build_type = build_type
    _accelize_drm.pytest_backend = backend
    _accelize_drm.pytest_fpga_driver = fpga_driver
    _accelize_drm.pytest_fpga_image = fpga_image
    _accelize_drm.pytest_hdk_version = hdk_version
    _accelize_drm.pytest_fpga_activators = fpga_activators
    _accelize_drm.pytest_ref_designs = ref_designs
    _accelize_drm.clean_nodelock_function = cleanNodelockEnv

    return _accelize_drm


class _Json:
    """Json file"""

    def __init__(self, tmpdir, name, content):
        self._path = str(tmpdir.join(name))
        self._content = content
        self._initial_content = deepcopy(content)
        self.save()

    def __delitem__(self, key):
        del self._content[key]

    def __setitem__(self, key, value):
        self._content[key] = value

    def __getitem__(self, key):
        return self._content[key]

    def __contains__(self, key):
        return key in self._content

    @property
    def path(self):
        """
        File path

        Returns:
            str: path
        """
        return self._path

    def save(self):
        """
        Save configuration in file.
        """
        with open(self._path, 'wt') as json_file:
            dump(self._content, json_file)

    def reset(self):
        """
        Reset configuration to initial content.
        """
        self._content = deepcopy(self._initial_content)
        self.save()


class ConfJson(_Json):
    """conf.json file"""

    def __init__(self, tmpdir, url, **kwargs):
        content = get_default_conf_json(url)
        for k, v in kwargs.items():
            content[k] = v
        _Json.__init__(self, tmpdir, 'conf.json', content)

    def addNodelock(self):
        self['licensing']['nodelocked'] = True
        self['licensing']['license_dir'] = dirname(self._path)
        assert isdir(self['licensing']['license_dir'])
        self.save()

    def removeNodelock(self):
        if 'nodelocked' in self['licensing']:
            del self['licensing']['nodelocked']
        assert 'nodelocked' not in self['licensing']
        if 'license_dir' in self['licensing'].keys():
            del self['licensing']['license_dir']
        assert 'license_dir' not in self['licensing']
        self.save()

    def cleanNodelockDir(self):
        from glob import glob
        dirpath = self['licensing']['license_dir']
        fileList = glob(join(dirpath, '*.req'))
        fileList.extend(glob(join(dirpath,'*.lic')))
        for e in fileList:
            remove(e)


class CredJson(_Json):
    """cred.json file"""

    def __init__(self, tmpdir, path):
        self._init_cref_path = path
        try:
            with open(path, 'rt') as cref_file:
                cred = load(cref_file)
        except OSError:
            cred = dict(client_id='', secret_id='')
        # Load from user specified cred.json
        _Json.__init__(self, tmpdir, 'cred.json', cred)
        self._user = 'default'

    def set_user(self, user=None):
        """
        Set user to use.

        Args:
            user (str): User to use. If not specified, use default user.
        """
        if user is None:
            self['client_id'] = self._initial_content['client_id']
            self['client_secret'] = self._initial_content['client_secret']
            self._user = 'default'
        else:
            try:
                self['client_id'] = self._initial_content['client_id_%s' % user]
                self['client_secret'] = self._initial_content['client_secret_%s' % user]
                self._user = user
            except KeyError:
                raise ValueError( 'User "%s" not found in "%s"' % (
                        user, self._init_cref_path))
        self.save()

    @property
    def user(self):
        return self._user

    @property
    def client_id(self):
        return self['client_id']

    @client_id.setter
    def client_id(self, s):
        self['client_id'] = s

    @property
    def client_secret(self):
        return self['client_secret']

    @client_secret.setter
    def client_secret(self, s):
        self['client_secret'] = s


@pytest.fixture
def conf_json(pytestconfig, tmpdir):
    """
    Manage "conf.json" in testing environment.
    """
    log_param = { 'log_verbosity': int(pytestconfig.getoption("library_verbosity")),
                  'log_format': int(pytestconfig.getoption("library_logformat")) }
    json_conf = ConfJson(tmpdir, pytestconfig.getoption("server"), settings=log_param)
    json_conf.save()
    return json_conf


@pytest.fixture
def cred_json(pytestconfig, tmpdir):
    """
    Manage "cred.json" in testing environment.
    """
    return CredJson(
        tmpdir, realpath(expanduser(pytestconfig.getoption("cred"))))


def _get_session_info():
    """
    Get session information in case of Tox run.

    Returns:
        dict: Session information.
    """
    if _SESSION.get('current_session_name') is None:

        current_session_name = environ.get('TOX_ENV_NAME')
        if current_session_name:

            backend, build_type = current_session_name.split('-')
            other_session_name = '-'.join((
                'c' if backend == 'cpp' else 'c', build_type))

            _SESSION.update(dict(
                current_session_name=current_session_name,
                current_session_lock=current_session_name + '.lock',
                other_session_name=other_session_name,
                other_session_lock=other_session_name + '.lock'))
    return _SESSION


def pytest_sessionstart(session):
    """
    Pytest session initialization

    Args:
        session (pytest.Session): Current Pytest session.
    """
    # Get session information
    current_session_lock = _get_session_info().get('current_session_name')

    if current_session_lock is None:
        return

    # Create lock to indicate session is running
    with open(current_session_lock, 'w'):
        pass


def pytest_sessionfinish(session):
    """
    Pytest session ending

    Args:
        session (pytest.Session): Current Pytest session.
    """
    # Get session information
    session_info = _get_session_info()
    current_session_lock = session_info.get('current_session_name')
    if current_session_lock is None:
        return

    # Delete lock to indicate session is terminated
    remove(current_session_lock)

    # If other session is also terminated, remove all locks
    if not isfile(session_info.get('other_session_lock')):
        for file in listdir('.'):
            if splitext(file)[1] == '.lock':
                remove(file)


def perform_once(test_name):
    """
    Function that skip test if already performed in another session of a Tox
    parallel run.

    Useful for tests that do not depends on the Python library backend.

    Args:
        test_name (str): Test name.

    Returns:
        function: Patched test function.
    """
    test_lock = test_name + '.lock'

    # Skip test if lock exists.
    if isfile(test_lock):
        pytest.skip(
            'Test "%s" already performed in another session.' % test_name)

    # Create lock
    else:
        with open(test_lock, 'w'):
            pass


class AsyncErrorHandler:
    """
    Asynchronous error callback
    """
    def __init__(self):
        self.reset()
    def reset(self):
        self.message = None
        self.errcode = None
        self.was_called = False
    def callback(self, message):
        self.was_called = True
        if isinstance(message, bytes):
            self.message = message.decode()
        else:
            self.message = message
        self.errcode = AsyncErrorHandlerList.get_error_code(self.message)
        #m = search(r'\[errCode=(\d+)\]', self.message)
        #if m:
        #    self.errcode = int(m.group(1))
        #else:
        #    self.errcode = None
    def assert_NoError( self, extra_msg=None ):
        if extra_msg is None:
            prepend_msg = ''
        else:
            prepend_msg = '%s: ' % extra_msg
        assert self.message is None, '%sAsynchronous callback reports a message: %s' % (prepend_msg, self.message)
        assert self.errcode is None, '%sAsynchronous callback returned error code: %d' % (prepend_msg, self.errcode)
        assert not self.was_called, '%sAsynchronous callback has been called' % prepend_msg


class AsyncErrorHandlerList(list):
    """
    Handle a list of asynchronous error callbacks and error message parsing
    """
    def create(self):
        cb = AsyncErrorHandler()
        super(AsyncErrorHandlerList, self).append(cb)
        return cb

    @staticmethod
    def get_error_code(msg):
        from re import search
        match = search(r'\[errCode=(\d+)\]', msg)
        assert match, "Could not find 'errCode' in exception message: %s" % msg
        return int(match.group(1))

    @staticmethod
    def get_error_details(msg):
        from re import search
        match = search(r'\{"error": "(.+)"\}', msg)
        if match:
            return match.group(1)
        else:
            return None


@pytest.fixture
def async_handler():
    return AsyncErrorHandlerList()



class WSAdmin:
    """
    Handle Web Service administration for test and debug of the DRM Lib
    """
    def __init__(self, url, client_id, client_secret):
        self._functions = WSListFunction( url, client_id, client_secret)

    def remove_product_information(self, product, user):
        self._functions._get_user_token()
        data = {'library': product['library'], 'name':product['name'], 'user':user}
        self._functions.remove_product_information(data)

    @property
    def functions(self):
        return self._functions


@pytest.fixture
def ws_admin(cred_json, conf_json):
    cred_json.set_user('admin')
    assert cred_json.user == 'admin'
    return WSAdmin(conf_json['licensing']['url'],
        cred_json['client_id'], cred_json['client_secret'])
