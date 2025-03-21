# ============================================================================ #
# Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #

import itertools
import random
import re
import uuid
from typing import Any, Optional

import uvicorn
from fastapi import FastAPI, Header, HTTPException, Query
from pydantic import BaseModel

# Define the REST Server App
app = FastAPI()


class Job(BaseModel):
    """Data required to submit a quantum job."""

    openQasm: str
    shots: int
    qbraidDeviceId: str


# Global variables to store job data and results
JOBS_MOCK_DB = {}
JOBS_MOCK_RESULTS = {}


def count_qubits(qasm: str) -> int:
    """Extracts the number of qubits from an OpenQASM string."""
    pattern = r"qreg\s+\w+\[(\d+)\];"

    match = re.search(pattern, qasm)

    if match:
        return int(match.group(1))

    raise ValueError("No qreg declaration found in the OpenQASM string.")


def simulate_job(qasm: str, num_shots: int) -> dict[str, int]:
    """Simulates a quantum job by generating random measurement outcomes."""
    num_qubits = count_qubits(qasm)

    all_states = ["".join(p) for p in itertools.product("01", repeat=num_qubits)]
    num_states_to_select = random.randint(1, len(all_states))
    selected_states = random.sample(all_states, num_states_to_select)
    distribution = random.choices(selected_states, k=num_shots)

    result = {state: distribution.count(state) for state in selected_states}

    return result


def poll_job_status(job_id: str) -> dict[str, Any]:
    """Updates the status of a job and returns the updated job data."""
    if job_id not in JOBS_MOCK_DB:
        raise HTTPException(status_code=404, detail="Job not found")

    status = JOBS_MOCK_DB[job_id]["status"]

    status_transitions = {
        "INITIALIZING": "QUEUED",
        "QUEUED": "RUNNING",
        "RUNNING": "COMPLETED",
        "CANCELLING": "CANCELLED",
    }

    new_status = status_transitions.get(status, status)
    JOBS_MOCK_DB[job_id]["status"] = new_status

    return {"qbraidJobId": job_id, **JOBS_MOCK_DB[job_id]}


@app.post("/quantum-jobs")
async def postJob(job: Job, api_key: Optional[str] = Header(None, alias="api-key")):
    """Submit a quantum job for execution."""
    if api_key is None:
        raise HTTPException(status_code=401, detail="API key is required")

    newId = str(uuid.uuid4())

    # TODO: Do this in a cuda-quantum way
    counts = simulate_job(job.openQasm, job.shots)

    job_data = {"status": "INITIALIZING", "statusText": "", **job.model_dump()}

    JOBS_MOCK_DB[newId] = job_data
    JOBS_MOCK_RESULTS[newId] = {"measurementCounts": counts}

    return {"qbraidJobId": newId, **job_data}


@app.get("/quantum-jobs")
async def getJobs(
    job_id: Optional[str] = Query(None, alias="qbraidJobId"),
    api_key: Optional[str] = Header(None, alias="api-key"),
):
    """Retrieve the status of one or more quantum jobs."""
    if api_key is None:
        raise HTTPException(status_code=401, detail="API key is required")

    jobs_array = []
    if job_id is None:
        for job in JOBS_MOCK_DB:
            job_data = poll_job_status(job)
            jobs_array.append(job_data)
    else:
        job_data = poll_job_status(job_id)
        jobs_array.append(job_data)

    res = {"jobsArray": jobs_array, "total": len(jobs_array)}

    return res


@app.get("/quantum-jobs/result/{job_id}")
async def getJobResult(job_id: str, api_key: Optional[str] = Header(None, alias="api-key")):
    """Retrieve the results of a quantum job."""
    if api_key is None:
        raise HTTPException(status_code=401, detail="API key is required")

    if job_id not in JOBS_MOCK_DB:
        raise HTTPException(status_code=404, detail="Job not found")

    if JOBS_MOCK_DB[job_id]["status"] in {"FAILED", "CANCELLED"}:
        raise HTTPException(
            status_code=409, detail="Results unavailable. Job failed or was cancelled."
        )

    if JOBS_MOCK_DB[job_id]["status"] != "COMPLETED":
        return {
            "error": "Job still in progress. Results will be available once job is completed.",
            "data": {},
        }

    if job_id not in JOBS_MOCK_RESULTS:
        raise HTTPException(status_code=500, detail="Job results not found")

    if random.random() < 0.2:
        return {"error": "Failed to retrieve job results. Please wait, and try again.", "data": {}}

    result = JOBS_MOCK_RESULTS[job_id]

    return result


def startServer(port):
    """Start the REST server."""
    uvicorn.run(app, port=port, host="0.0.0.0", log_level="info")


if __name__ == "__main__":
    startServer(62447)
