"""Gemini API client (placeholder until milestone 8).

Role: called by server.py for /api/explain and /api/health.
"""

import os


class GeminiError(Exception):
    def __init__(self, message, status=502):
        super().__init__(message)
        self.status = status


def status():
    return {"configured": bool(os.environ.get("GEMINI_API_KEY")), "model": os.environ.get("GEMINI_MODEL") or None}


def explain(request):
    raise GeminiError("The Gemini integration is not implemented yet.", 501)
