import os
from google.cloud import storage

def upload_blob(bucket_name, source_folder, destination_folder):
    storage_client = storage.Client()
    bucket = storage_client.bucket(bucket_name)
    
    for local_root, dirs, files in os.walk(source_folder):
        for filename in files:
            if 'script' not in local_root:
                local_path = os.path.join(local_root, filename)
                relative_path = os.path.relpath(local_path, source_folder)
                blob_path = os.path.join(destination_folder, relative_path)
                
                blob = bucket.blob(blob_path)

                # Correct way to check if blob exists
                if not blob.exists(storage_client):
                    print(f"Uploading {local_path} to {blob_path}")
                    blob.upload_from_filename(local_path)
                else:
                    print(f"{blob_path} already exists, skipping upload.")

# Example usage:
bucket_name = 'compass_osdi'
source_folder = str(__import__('pathlib').Path(__file__).resolve().parents[2] / 'data/compass/upstream/dataset')
destination_folder = 'dataset/'

upload_blob(bucket_name, source_folder, destination_folder)