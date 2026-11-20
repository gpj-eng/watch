"""Optional Tencent Cloud SMS sender using the official Python SDK."""

import json
import logging
import os

from protocol import valid_phone

LOG = logging.getLogger("hspi")


def env_required(name):
    value = os.environ.get(name, "").strip()
    if not value:
        raise ValueError("Missing environment variable " + name)
    return value


class TencentSms:
    def __init__(self):
        from tencentcloud.common.credential import Credential
        from tencentcloud.common.profile.client_profile import ClientProfile
        from tencentcloud.common.profile.http_profile import HttpProfile
        from tencentcloud.sms.v20210111 import models, sms_client

        self.allowed = set(env_required("HSPI_SMS_ALLOWED_NUMBERS").split(","))
        if not all(valid_phone(phone) for phone in self.allowed):
            raise ValueError("HSPI_SMS_ALLOWED_NUMBERS must contain comma-separated E.164 numbers")
        self.app_id = env_required("TENCENT_SMS_APP_ID")
        self.sign = env_required("TENCENT_SMS_SIGN")
        self.template = env_required("TENCENT_SMS_TEMPLATE_ID")
        self.params = json.loads(os.environ.get("TENCENT_SMS_TEMPLATE_PARAMS", "[]"))
        if not isinstance(self.params, list) or not all(isinstance(p, str) for p in self.params):
            raise ValueError("TENCENT_SMS_TEMPLATE_PARAMS must be a JSON list of strings")
        credentials = Credential(env_required("TENCENTCLOUD_SECRET_ID"),
                                 env_required("TENCENTCLOUD_SECRET_KEY"))
        http = HttpProfile()
        http.endpoint = "sms.tencentcloudapi.com"
        http.reqTimeout = 10
        profile = ClientProfile()
        profile.httpProfile = http
        self.client = sms_client.SmsClient(
            credentials, os.environ.get("TENCENT_SMS_REGION", "ap-guangzhou"), profile
        )
        self.request_class = models.SendSmsRequest

    def send(self, event_id, reason, phones):
        if any(phone in {"+12025550100", "+12025550101", "+12025550102"} for phone in phones):
            LOG.warning("SOS %016x blocked: reserved bench number", event_id)
            return False
        if not set(phones).issubset(self.allowed):
            LOG.warning("SOS %016x blocked: destination outside local allowlist", event_id)
            return False
        request = self.request_class()
        request.PhoneNumberSet = list(phones)
        request.SmsSdkAppId = self.app_id
        request.SignName = self.sign
        request.TemplateId = self.template
        request.TemplateParamSet = self.params
        request.SessionContext = "{:016x}".format(event_id)
        try:
            result = self.client.SendSms(request)
        except Exception as exc:
            # The exception might include recipients, request parameters or credentials.
            LOG.error("SOS %016x: SMS request outcome uncertain (%s); check provider history.",
                      event_id, type(exc).__name__)
            return False
        accepted = {status.PhoneNumber for status in (result.SendStatusSet or [])
                    if status.Code == "Ok"}
        count = len(accepted.intersection(phones))
        LOG.info("SOS %016x: provider accepted %d/%d recipients", event_id, count, len(phones))
        return count == len(phones)
