#pragma once

// This first policy accepts only versions sent through the supervisor's
// private endpoint. The sender capability is the authorization decision.
enum { MOSS_CODE_APPROVE = 1, MOSS_CODE_OK = 0, MOSS_CODE_BAD_REQUEST = 1, MOSS_CODE_UNAVAILABLE = 2 };
