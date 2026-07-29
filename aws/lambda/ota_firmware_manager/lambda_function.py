import json
import os
import boto3
from botocore.exceptions import ClientError


# ============================================================
# AWS configuration
# ============================================================

AWS_REGION = os.environ.get("AWS_REGION", "us-east-1")

FIRMWARE_BUCKET = os.environ.get(
    "FIRMWARE_BUCKET",
    "esp32-ota-firmware-2026-157320904947-us-east-1-an"
)

FIRMWARE_KEY = os.environ.get(
    "FIRMWARE_KEY",
    "firmware/firmware_v2.bin"
)

LATEST_VERSION = os.environ.get(
    "LATEST_VERSION",
    "2.0.0"
)

PRESIGNED_URL_EXPIRATION = int(
    os.environ.get("URL_EXPIRATION", "3600")
)


# ============================================================
# AWS clients
# ============================================================

s3_client = boto3.client(
    "s3",
    region_name=AWS_REGION
)


# ============================================================
# HTTP response helper
# ============================================================

def create_response(status_code, body):
    return {
        "statusCode": status_code,
        "headers": {
            "Content-Type": "application/json",
            "Access-Control-Allow-Origin": "*",
            "Cache-Control": "no-store"
        },
        "body": json.dumps(body)
    }


# ============================================================
# Read query parameters sent by ESP32
# ============================================================

def get_query_parameters(event):
    query = event.get("queryStringParameters")

    if not isinstance(query, dict):
        query = {}

    device_id = query.get("device_id", "unknown")
    current_version = query.get("current_version", "0.0.0")

    return device_id, current_version


# ============================================================
# Version comparison
# ============================================================

def version_to_tuple(version):
    try:
        clean_version = str(version).strip().lstrip("vV")

        return tuple(
            int(part)
            for part in clean_version.split(".")
        )

    except (ValueError, TypeError):
        return (0, 0, 0)


def is_newer_version(latest_version, current_version):
    return (
        version_to_tuple(latest_version)
        >
        version_to_tuple(current_version)
    )


# ============================================================
# Lambda entry point
# ============================================================

def lambda_handler(event, context):
    try:
        print(
            "Received event:",
            json.dumps(event)
        )

        device_id, current_version = (
            get_query_parameters(event)
        )

        print(f"Device ID: {device_id}")
        print(f"Current version: {current_version}")
        print(f"Latest version: {LATEST_VERSION}")

        # ----------------------------------------------------
        # Verify that the firmware file exists in S3
        # ----------------------------------------------------

        firmware_metadata = s3_client.head_object(
            Bucket=FIRMWARE_BUCKET,
            Key=FIRMWARE_KEY
        )

        firmware_size = firmware_metadata.get(
            "ContentLength",
            0
        )

        firmware_etag = firmware_metadata.get(
            "ETag",
            ""
        ).replace('"', '')

        # ----------------------------------------------------
        # Compare firmware versions
        # ----------------------------------------------------

        update_available = is_newer_version(
            LATEST_VERSION,
            current_version
        )

        download_url = None

        if update_available:
            download_url = (
                s3_client.generate_presigned_url(
                    ClientMethod="get_object",
                    Params={
                        "Bucket": FIRMWARE_BUCKET,
                        "Key": FIRMWARE_KEY,
                        "ResponseContentType":
                            "application/octet-stream"
                    },
                    ExpiresIn=PRESIGNED_URL_EXPIRATION
                )
            )

        firmware_status = (
            "available"
            if update_available
            else "up_to_date"
        )

        # ----------------------------------------------------
        # Response expected by ESP32
        # ----------------------------------------------------

        response_body = {
            "ready": True,
            "status": "ready",
            "device_id": device_id,
            "current_version": current_version,
            "latest_version": LATEST_VERSION,
            "update_available": update_available,
            "firmware_status": firmware_status,
            "firmware_key": FIRMWARE_KEY,
            "firmware_size": firmware_size,
            "firmware_etag": firmware_etag,
            "download_url": download_url,
            "expires_in": (
                PRESIGNED_URL_EXPIRATION
                if update_available
                else 0
            )
        }

        print(
            "Successful response:",
            json.dumps({
                **response_body,
                "download_url": (
                    "generated"
                    if download_url
                    else None
                )
            })
        )

        return create_response(
            200,
            response_body
        )

    except ClientError as error:
        error_code = (
            error.response
            .get("Error", {})
            .get("Code", "Unknown")
        )

        print(f"AWS ClientError: {error}")

        if error_code in (
            "404",
            "NoSuchKey",
            "NotFound"
        ):
            return create_response(
                404,
                {
                    "ready": False,
                    "status": "error",
                    "update_available": False,
                    "firmware_status": "not_found",
                    "message": "Firmware file not found in S3",
                    "firmware_key": FIRMWARE_KEY
                }
            )

        if error_code == "AccessDenied":
            return create_response(
                403,
                {
                    "ready": False,
                    "status": "error",
                    "update_available": False,
                    "firmware_status": "access_denied",
                    "message": (
                        "Lambda does not have permission "
                        "to access the firmware file"
                    )
                }
            )

        return create_response(
            500,
            {
                "ready": False,
                "status": "error",
                "update_available": False,
                "firmware_status": "aws_error",
                "message": str(error)
            }
        )

    except Exception as error:
        print(f"Unexpected error: {error}")

        return create_response(
            500,
            {
                "ready": False,
                "status": "error",
                "update_available": False,
                "firmware_status": "internal_error",
                "message": str(error)
            }
        )