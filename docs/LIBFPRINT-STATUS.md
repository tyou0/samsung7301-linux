# libfprint driver status

The driver in [`../contrib/samsung7301.c`](../contrib/samsung7301.c) is
complete and working:

| operation | result |
|---|---|
| device discovery | `fprintd-list` shows the sensor |
| enrolment | `fprintd-enroll $USER right-index-finger` completes 5 stages |
| verification | `fprintd-verify $USER` reports `verify-match` |
| `sudo` | prompts "Place your right index finger on the fingerprint reader" |

The templates are stored by fprintd in its own local storage
(`/var/lib/fprint/$USER/samsung7301/0/<finger>`); the sensor has no on-device
template storage that is reachable through this protocol, so libfprint does
the matching in software.

See [`LIBFPRINT.md`](LIBFPRINT.md) for the build and install instructions and
for the implementation notes.
