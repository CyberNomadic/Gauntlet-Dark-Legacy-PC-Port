"""Fetch the manifest-pinned vcpkg checkout inside the disposable build stage."""
import json
import subprocess


def main():
    with open('/source/vcpkg.json', encoding='utf-8') as manifest:
        revision = json.load(manifest)['builtin-baseline']
    subprocess.run(['git', 'clone', '--filter=blob:none',
                    'https://github.com/microsoft/vcpkg', '/opt/vcpkg'], check=True)
    subprocess.run(['git', '-C', '/opt/vcpkg', 'checkout', revision], check=True)


if __name__ == '__main__':
    main()
