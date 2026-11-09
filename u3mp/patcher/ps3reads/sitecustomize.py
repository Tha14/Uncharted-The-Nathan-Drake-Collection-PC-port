# The import's PS3 read list (u3mp_ps3reads.py): active only when u3mp_import.py set U3MP_PS3_ROOTS / U3MP_PS3_READS.
import os

if os.environ.get('U3MP_PS3_ROOTS') and os.environ.get('U3MP_PS3_READS'):
    try:
        import u3mp_ps3reads
        u3mp_ps3reads.install()
    except Exception:
        pass
