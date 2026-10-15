"""Real BLE command handler and C storage; radio/display/task calls are stubbed."""
import asyncio
import ctypes as ct
import json
import pytest
from test_app_install import firmware  # noqa: F401
from app_package import app_request, build_package, download_app, install_app


@pytest.fixture
def protocol(firmware, tmp_path):
    lib = firmware
    lib.app_test_set_root.argtypes = [ct.c_char_p]
    lib.app_test_set_job.argtypes = [ct.c_char_p]
    lib.app_test_set_root(str(tmp_path).encode())
    lib.ble_host_apps_handle.argtypes = [ct.c_char_p, ct.c_void_p, ct.POINTER(ct.c_void_p)]
    class Bridge:
        async def request(self, request, timeout=None):
            if request['op'] == 'hello':
                return {'ok': True, 'data': {'ops': ['app_begin', 'app_write', 'app_commit', 'app_abort']}}
            obj = lib.cJSON_Parse(json.dumps(request).encode())
            output = ct.c_void_p()
            try:
                code = lib.ble_host_apps_handle(request['op'].encode(), obj, ct.byref(output))
                assert lib.app_test_locks() == 0
                return {'ok': code == 0, 'data': json.loads(ct.string_at(output)) if output.value else {}, 'error': str(code)}
            finally:
                lib.cJSON_Delete(obj)
                if output.value:
                    lib.cJSON_free(output)
    return Bridge(), lib, tmp_path


def test_protocol_install_download_rename_remove(protocol):
    bridge, lib, root = protocol
    package = build_package('print("hello")', 'managed', '用户应用', selected=__import__('pathlib').Path(__file__).parent/'assets/app_selected.bin')
    result = asyncio.run(install_app(bridge, package))
    assert result == {'saved': True, 'refresh': 'when_home'}
    assert asyncio.run(download_app(bridge, 'managed')) == package
    assert asyncio.run(app_request(bridge, 'app_info', app_id='managed'))['directory'] == 'apps/g00000000/managed'
    asyncio.run(app_request(bridge, 'app_rename', app_id='managed', title='新名称'))
    assert asyncio.run(app_request(bridge, 'app_info', app_id='managed'))['manifest']['title'] == '新名称'
    for busy in ['display', 'background']:
        lib.app_test_set_exclusive(busy == 'display')
        lib.app_test_set_job(str(root/'apps/g00000000/managed/main.lua').encode() if busy == 'background' else b'')
        with pytest.raises(RuntimeError):
            asyncio.run(app_request(bridge, 'app_remove', app_id='managed'))
    lib.app_test_set_exclusive(0)
    lib.app_test_set_job(b'')
    asyncio.run(app_request(bridge, 'app_remove', app_id='managed'))
    assert asyncio.run(app_request(bridge, 'app_list')) == []
    assert lib.app_test_refreshes() == 3


@pytest.mark.parametrize('command', [
    {'op':'app_write','app_id':'bad','file':'../main.lua','offset':0,'data':'eA=='},
    {'op':'app_write','app_id':'bad','file':'main.lua','offset':-1,'data':'eA=='},
    {'op':'app_read','app_id':'bad','file':'main.lua','offset':0,'length':1025},
    {'op':'app_remove','app_id':'../system'},
    {'op':'app_rename','app_id':'bad','title':''},
    {'op':'app_list','offset':0.5},
    {'op':'app_unknown'},
])
def test_protocol_rejects_invalid_requests(protocol, command):
    bridge, lib, root = protocol
    assert not asyncio.run(bridge.request(command))['ok']
    assert not (root/'system').exists()
    assert lib.app_test_refreshes() == 0
