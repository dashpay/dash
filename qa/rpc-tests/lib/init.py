import sys
import os
sys.path.append(os.path.join(os.path.dirname(__file__), '..'))
sys.path.append(os.path.join(os.path.dirname(__file__), '..', 'lib'))


def is_valid_python_version():
    version_valid = False

    ver = sys.version_info
    if (2 == ver.major) and (7 <= ver.minor):
        version_valid = True

    if (3 == ver.major) and (4 <= ver.minor):
        version_valid = True

    return version_valid


def python_short_ver_str():
    ver = sys.version_info
    return "%s.%s" % (ver.major, ver.minor)


def are_deps_installed():
    installed = False

    try:
        import peewee
        import bitcoinrpc.authproxy
        import simplejson
        import inflection
        installed = True
    except ImportError as e:
        print("[error]: Missing dependencies")

    return installed


def is_database_correctly_configured():
    import peewee
    import config

    configured = False

    cannot_connect_message = "Cannot connect to database. Please ensure database service is running and user access is properly configured in 'sentinel.conf'."

    try:
        db = config.db
        db.connect()
        configured = True
    except (peewee.ImproperlyConfigured, peewee.OperationalError, ImportError) as e:
        print("[error]: %s" % e)
        print(cannot_connect_message)
        sys.exit(1)

    return configured


def has_dash_conf():
    import config
    import io

    valid_dash_conf = False

    # ensure dash_conf exists & readable
    #
    # if not, print a message stating that Dash Core must be installed and
    # configured, including JSONRPC access in dash.conf
    try:
        f = io.open(config.dash_conf)
        valid_dash_conf = True
    except IOError as e:
        print(e)

    return valid_dash_conf


# === begin main


def main():
    install_instructions = "\tpip install -r requirements.txt"

    if not is_valid_python_version():
        print("Python %s is not supported" % python_short_ver_str())
        sys.exit(1)

    if not are_deps_installed():
        print("Please ensure all dependencies are installed:")
        print(install_instructions)
        sys.exit(1)

    if not is_database_correctly_configured():
        print("Please ensure correct database configuration.")
        sys.exit(1)

    #~ if not has_dash_conf():
        #~ print("DashCore must be installed and configured, including JSONRPC access in dash.conf")
        #~ sys.exit(1)


main()
