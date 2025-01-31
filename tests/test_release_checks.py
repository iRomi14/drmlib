# -*- coding: utf-8 -*-
"""
Performs some pre-release checks
"""
import pytest
from tests.conftest import perform_once


def test_changelog_and_version(accelize_drm):
    """
    Checks if Version match with Git tag and if changelog is up to date.
    """
    perform_once(__name__ + '.test_changelog_and_version')

    from os.path import join
    from subprocess import run, PIPE
    from re import fullmatch

    if not accelize_drm.pytest_build_environment:
        pytest.skip("Can only be checked in build environment")

    # Ensure tags are pulled
    try:
        run(['git', 'fetch', '--tags', '--force'],
            stderr=PIPE, stdout=PIPE, universal_newlines=True)
    except FileNotFoundError:
        fail = (
            pytest.fail if accelize_drm.pytest_build_type == 'debug' else
            pytest.xfail)
        fail('Git is required for this test.')

    # Get head tag if any
    result = run(['git', 'describe', '--abbrev=0', '--exact-match', '--tags',
                  'HEAD'], stderr=PIPE, stdout=PIPE, universal_newlines=True)

    if result.returncode:
        pytest.skip("Can only be checked on tagged git head")

    tag = result.stdout.strip()
    version = tag.lstrip('v')

    # Checks tag format using library version
    lib_ver = accelize_drm.get_api_version()
    assert tag == 'v%s' % (lib_ver.version.split('+')[0])

    # Check tag format match semantic versioning

    if not fullmatch(r'^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)'
                     r'(-(0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*)'
                     r'(\.(0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*))*)?'
                     r'(\+[0-9a-zA-Z-]+(\.[0-9a-zA-Z-]+)*)?$', version):
        pytest.fail('"%s" does not match semantic versioning format.' % version)

    # Check if changelog is up-to-date (Not for prereleases)
    if not lib_ver.prerelease:
        changelog_path = join(accelize_drm.pytest_build_source_dir, 'CHANGELOG')
        with open(changelog_path, 'rt') as changelog:
            last_change = changelog.readline().strip()

        assert fullmatch(
            r"\* [a-zA-Z]{3} [a-zA-Z]{3} [0-9]{2} [0-9]{4} Accelize " + tag,
            last_change)

    # Check prerelease format:
    # Alpha: "1.0.0-alpha.1"
    # Beta: "1.0.0-beta.1"
    # Release candidate: "1.0.0-rc.1"
    else:
        assert fullmatch(r"(alpha|beta|rc)\.[0-9]+", lib_ver.prerelease)
