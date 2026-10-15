"""Run real C storage under ASan/UBSan (macOS/Homebrew or Linux OpenSSL)."""
from pathlib import Path
import os
import subprocess
import tempfile

component = Path(__file__).resolve().parents[1]
root = component.parents[2]
cjson = Path(os.environ.get('APP_TEST_CJSON', str(root / 'application/edge_agent/managed_components/espressif__cjson/cJSON')))
openssl = Path(os.environ.get('APP_TEST_OPENSSL', '/opt/homebrew/opt/openssl@3'))
with tempfile.TemporaryDirectory(prefix='app-store-asan-') as tmp:
    binary = Path(tmp) / 'smoke'
    subprocess.run(['clang', '-g', '-O1', '-fsanitize=address,undefined', '-DLOCAL_APP_HOST',
                    '-I' + str(component/'include'), '-I' + str(component/'test_host'),
                    '-I' + str(cjson), '-I' + str(openssl/'include'), '-L' + str(openssl/'lib'),
                    str(component/'src/local_app_store.c'), str(component/'src/local_app_index.c'),
                    str(component/'test_host/sanitizer_smoke.c'), str(cjson/'cJSON.c'),
                    '-lcrypto', '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(Path(tmp)/'apps')], check=True)
