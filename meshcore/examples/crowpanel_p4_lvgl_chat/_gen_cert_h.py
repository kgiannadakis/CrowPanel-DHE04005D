cert = open('dash_cert.pem').read()
key  = open('dash_key.pem').read()
def esc(s):
    return '\n'.join('"' + line + '\\n"' for line in s.splitlines())
out = (
    '// Auto-generated self-signed cert for the HTTPS dashboard (port 443).\n'
    '// Regenerate via:\n'
    '//   openssl req -x509 -newkey rsa:2048 -sha256 -nodes \\\n'
    '//     -keyout dash_key.pem -out dash_cert.pem -days 3650 \\\n'
    '//     -subj "/CN=meshcore.local/O=MeshCore/C=US"\n'
    '//   then python _gen_cert_h.py && rm dash_*.pem\n'
    '// Browsers warn on first visit (self-signed) - accept and continue.\n'
    '// The same cert/key ships with every build; nothing secret.\n'
    '#pragma once\n'
    '\n'
    'static const char DASH_CERT_PEM[] =\n'
    + esc(cert) + ';\n'
    '\n'
    'static const char DASH_KEY_PEM[] =\n'
    + esc(key) + ';\n'
)
open('dashboard_cert.h', 'w').write(out)
print('wrote dashboard_cert.h')
